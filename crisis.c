/*
 * Practica 2 de Sistemas Operativos I: crisis.c
 * Simulacion de una empresa matriz (padre) con filiales (hijos, nietos...)
 *
 * Uso: crisis n_max_procs [normal|veloz]
 *
 * Notas de diseno:
 *  - El fichero compartido "crisis.dat" guarda dos enteros:
 *        datos[0] = numero maximo de procesos
 *        datos[1] = numero de procesos vivos (incluido el padre)
 *  - El contador se INCREMENTA antes del fork (reservamos el hueco) y se
 *    DECREMENTA cuando el padre de la empresa muerta hace wait. Asi nunca
 *    se libera un hueco hasta estar seguros de que el proceso ha muerto.
 *  - Toda la zona critica (leer/modificar el fichero) se hace con lockf y
 *    con las señales bloqueadas, para que una manejadora no se cuele en
 *    medio.
 *  - Solo el padre imprime V/M para las filiales; el padre no imprime nada
 *    al nacer ni al morir salvo el mensaje final.
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define FICHERO "crisis.dat"
#define MAX_PROCS 34

/* Variables estaticas (las necesitan las manejadoras) */
static int fd;                    /* descriptor del fichero compartido */
static int soy_padre = 1;         /* 1 si somos la empresa matriz */
static pid_t hijos[MAX_PROCS];    /* PIDs de nuestras filiales vivas (0 = hueco) */

/* ------------------------------------------------------------------ */
/* Funciones auxiliares sencillas                                      */
/* ------------------------------------------------------------------ */

/* Imprime "L(pid)\n" con un unico write (L es V o M) */
static void imprimir_evento(char letra, pid_t pid)
{
    char buf[32];
    char digitos[16];
    int n = 0;
    int i = 0;
    long v = pid;

    if (v == 0)
        digitos[n++] = '0';
    while (v > 0) {                 /* sacamos los digitos al reves */
        digitos[n++] = '0' + v % 10;
        v = v / 10;
    }
    buf[i++] = letra;
    buf[i++] = '(';
    while (n > 0)                   /* y los ponemos en orden */
        buf[i++] = digitos[--n];
    buf[i++] = ')';
    buf[i++] = '\n';
    write(1, buf, i);
}

/* Compara dos cadenas: devuelve 1 si son iguales y 0 si no */
static int iguales(const char *a, const char *b)
{
    int i = 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == b[i];
}

/* Duerme n segundos completos aunque lleguen señales por el camino */
static void dormir(int n)
{
    while (n > 0)
        n = sleep(n);
}

/* Bloquea el fichero entero (el puntero se pone antes al principio.
 * Es seguro porque cada proceso tiene su propio descriptor). */
static void bloquear(void)
{
    lseek(fd, 0, SEEK_SET);
    lockf(fd, F_LOCK, 0);
}

/* Desbloquea el fichero entero */
static void desbloquear(void)
{
    lseek(fd, 0, SEEK_SET);
    lockf(fd, F_ULOCK, 0);
}

/* Lee los dos enteros del fichero (hay que tenerlo bloqueado) */
static void leer_datos(int datos[2])
{
    lseek(fd, 0, SEEK_SET);
    read(fd, datos, 2 * sizeof(int));
}

/* Escribe los dos enteros en el fichero (hay que tenerlo bloqueado) */
static void escribir_datos(int datos[2])
{
    lseek(fd, 0, SEEK_SET);
    write(fd, datos, 2 * sizeof(int));
}

/* Resta uno al numero de procesos vivos. Solo se llama desde
 * manejadoras o con las señales bloqueadas. */
static void decrementar_vivos(void)
{
    int datos[2];

    bloquear();
    leer_datos(datos);
    datos[1]--;
    escribir_datos(datos);
    desbloquear();
}

/* ------------------------------------------------------------------ */
/* Manejadoras de señales                                              */
/* ------------------------------------------------------------------ */

/* SIGCHLD: una filial ha muerto. La recogemos (sin quedarnos zombie),
 * la quitamos de la lista y liberamos su hueco en el fichero. */
static void manejadora_sigchld(int sig)
{
    int estado;
    pid_t pid;
    int i;

    while ((pid = waitpid(-1, &estado, WNOHANG)) > 0) {
        for (i = 0; i < MAX_PROCS; i++)
            if (hijos[i] == pid)
                hijos[i] = 0;
        decrementar_vivos();
    }
}

/* SIGTERM (solo filiales): la empresa quiebra, y con ella todas sus
 * descendientes. Mandamos SIGTERM a las filiales, esperamos a que
 * mueran todas, imprimimos M y morimos. */
static void manejadora_sigterm(int sig)
{
    int estado;
    int i;

    for (i = 0; i < MAX_PROCS; i++)
        if (hijos[i] != 0)
            kill(hijos[i], SIGTERM);

    /* Esperamos a todas las filiales (y liberamos sus huecos) */
    while (waitpid(-1, &estado, 0) > 0)
        decrementar_vivos();

    imprimir_evento('M', getpid());
    exit(0);
}

/* SIGINT (solo padre): fin ordenado de la practica (CTRL-C) */
static void manejadora_sigint(int sig)
{
    int estado;
    int i;
    static const char fin[] = "Programa acabado correctamente\n";

    for (i = 0; i < MAX_PROCS; i++)
        if (hijos[i] != 0)
            kill(hijos[i], SIGTERM);

    /* Esperamos a que hayan muerto todas las empresas */
    while (waitpid(-1, &estado, 0) > 0)
        ;

    write(1, fin, sizeof(fin) - 1);
    close(fd);
    unlink(FICHERO);                /* borramos el fichero temporal */
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Creacion de una filial                                              */
/* ------------------------------------------------------------------ */

/* Intenta crear una filial. Si hay hueco, reserva su sitio en el
 * fichero y hace fork. Si somos el hijo, deja preparado su estado y
 * vuelve (el hijo continuara en el mismo bucle principal). */
static void crear_filial(void)
{
    sigset_t bloqueo, viejo;
    int datos[2];
    int puede = 0;
    pid_t pid;
    int i;
    struct sigaction accion;

    /* Bloqueamos señales mientras comprobamos, hacemos fork y apuntamos
     * el PID, para que ninguna manejadora se meta a medias. */
    sigemptyset(&bloqueo);
    sigaddset(&bloqueo, SIGINT);
    sigaddset(&bloqueo, SIGTERM);
    sigaddset(&bloqueo, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bloqueo, &viejo);

    /* Zona critica: comprobar si cabe otro proceso y reservar el hueco */
    bloquear();
    leer_datos(datos);
    if (datos[1] < datos[0]) {
        datos[1]++;
        escribir_datos(datos);
        puede = 1;
    }
    desbloquear();

    if (!puede) {                   /* no cabe: no creamos nada */
        sigprocmask(SIG_SETMASK, &viejo, NULL);
        return;
    }

    pid = fork();

    if (pid == -1) {                /* fallo del fork: devolvemos el hueco */
        decrementar_vivos();
        sigprocmask(SIG_SETMASK, &viejo, NULL);
        return;
    }

    if (pid == 0) {
        /* ---- Codigo del hijo ---- */
        soy_padre = 0;
        for (i = 0; i < MAX_PROCS; i++)   /* no heredamos las filiales del padre */
            hijos[i] = 0;
        srand(getpid());                  /* semilla distinta en cada proceso */
        imprimir_evento('V', getpid());   /* nada mas nacer */

        close(fd);                        /* cerramos el descriptor heredado */
        fd = open(FICHERO, O_RDWR);       /* y lo abrimos de nuevo (sin O_TRUNC) */

        /* SIGTERM: quiebra con sus descendientes */
        accion.sa_handler = manejadora_sigterm;
        sigemptyset(&accion.sa_mask);
        sigaddset(&accion.sa_mask, SIGINT);
        sigaddset(&accion.sa_mask, SIGCHLD);
        accion.sa_flags = 0;
        sigaction(SIGTERM, &accion, NULL);

        /* SIGINT: lo ignoramos, el CTRL-C lo gestiona el padre */
        accion.sa_handler = SIG_IGN;
        sigemptyset(&accion.sa_mask);
        accion.sa_flags = 0;
        sigaction(SIGINT, &accion, NULL);

        sigprocmask(SIG_SETMASK, &viejo, NULL);
        return;
    }

    /* ---- Codigo del padre: apuntamos el PID de la filial ---- */
    for (i = 0; i < MAX_PROCS; i++) {
        if (hijos[i] == 0) {
            hijos[i] = pid;
            break;
        }
    }
    sigprocmask(SIG_SETMASK, &viejo, NULL);
}

/* ------------------------------------------------------------------ */
/* Programa principal                                                  */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static const char uso[] = "Uso: crisis n_max_procs [normal|veloz]\n";
    static const char error_num[] = "n_max_procs debe ser un entero entre 3 y 34\n";
    static const char error_vel[] = "La velocidad debe ser normal o veloz\n";
    int n_max = 0;
    int veloz = 0;
    int i;
    int datos[2];
    struct sigaction accion;

    /* --- Comprobacion de argumentos --- */
    if (argc != 2 && argc != 3) {
        write(2, uso, sizeof(uso) - 1);
        exit(1);
    }

    /* Convertimos el primer argumento a entero a mano */
    if (argv[1][0] == '\0') {
        write(2, error_num, sizeof(error_num) - 1);
        exit(1);
    }
    for (i = 0; argv[1][i] != '\0'; i++) {
        if (argv[1][i] < '0' || argv[1][i] > '9' || n_max > MAX_PROCS) {
            write(2, error_num, sizeof(error_num) - 1);
            exit(1);
        }
        n_max = n_max * 10 + (argv[1][i] - '0');
    }
    if (n_max < 3 || n_max > MAX_PROCS) {
        write(2, error_num, sizeof(error_num) - 1);
        exit(1);
    }

    /* Segundo argumento (opcional) */
    if (argc == 3) {
        if (iguales(argv[2], "veloz"))
            veloz = 1;
        else if (!iguales(argv[2], "normal")) {
            write(2, error_vel, sizeof(error_vel) - 1);
            exit(1);
        }
    }

    /* --- Fichero compartido: maximo y numero de vivos (solo el padre) --- */
    fd = open(FICHERO, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd == -1)
        exit(1);
    datos[0] = n_max;
    datos[1] = 1;                   /* el padre ya cuenta */
    escribir_datos(datos);

    for (i = 0; i < MAX_PROCS; i++)
        hijos[i] = 0;

    srand(getpid());

    /* --- Manejadoras del padre (los hijos heredan SIGCHLD) --- */
    accion.sa_handler = manejadora_sigchld;
    sigemptyset(&accion.sa_mask);
    sigaddset(&accion.sa_mask, SIGINT);
    sigaddset(&accion.sa_mask, SIGTERM);
    accion.sa_flags = 0;
    sigaction(SIGCHLD, &accion, NULL);

    accion.sa_handler = manejadora_sigint;
    sigemptyset(&accion.sa_mask);
    sigaddset(&accion.sa_mask, SIGCHLD);
    sigaddset(&accion.sa_mask, SIGTERM);
    accion.sa_flags = 0;
    sigaction(SIGINT, &accion, NULL);

    /* --- Bucle infinito de la vida de cada empresa --- */
    for (;;) {
        /* 1. Pausa al azar entre 1 y 4 segundos (solo en modo normal) */
        if (!veloz)
            dormir(1 + rand() % 4);

        if (soy_padre) {
            /* 2. La matriz intenta crear una filial */
            crear_filial();
        } else {
            /* 3. Una filial: 50% quiebra, 50% crea otra filial */
            if (rand() % 2 == 0)
                raise(SIGTERM);     /* quiebra: la manejadora hace el resto */
            else
                crear_filial();
        }
        /* 5. Si no se pudo crear, volvemos a empezar el bucle */
    }

    return 0;
}
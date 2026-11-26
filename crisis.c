#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define FICHERO "crisis.dat"
#define MAX_PROCS 34

static int fd;                 
static int soy_padre = 1;         
static pid_t hijos[MAX_PROCS];    

static void imprimir_evento(char letra, pid_t pid){
    char texto[32];
    char digitos[16];
    int n = 0;
    int i = 0;
    long numero = pid;

    if (numero == 0){
        digitos[n++] = '0';
    }
        
    while (numero > 0) {                
        digitos[n++] = '0' + numero % 10;
        numero = numero / 10;
    }
    texto[i++] = letra;
    texto[i++] = '(';
    while (n > 0){
        texto[i++] = digitos[--n];
    }                  
        
    texto[i++] = ')';
    texto[i++] = '\n';
    write(1, texto, i);
}

static int iguales(const char *cadena1, const char *cadena2){
    int i = 0;

    while (cadena1[i] != '\0' && cadena2[i] != '\0') {
        if (cadena1[i] != cadena2[i]){
            return 0;
        }
        i++;
    }
    return cadena1[i] == cadena2[i];
}

static void dormir(int segundos){
    while (segundos > 0)
        segundos = sleep(segundos);
}

static void bloquear(void){
    lseek(fd, 0, SEEK_SET);
    lockf(fd, F_LOCK, 0);
}

static void desbloquear(void){
    lseek(fd, 0, SEEK_SET);
    lockf(fd, F_ULOCK, 0);
}

static void leer_datos(int datos[2]){
    lseek(fd, 0, SEEK_SET);
    read(fd, datos, 2 * sizeof(int));
}

static void escribir_datos(int datos[2])
{
    lseek(fd, 0, SEEK_SET);
    write(fd, datos, 2 * sizeof(int));
}

static void decrementar_vivos(void){
    int datos[2];

    bloquear();
    leer_datos(datos);
    datos[1]--; 
    escribir_datos(datos);
    desbloquear();
}

static void manejadora_sigchld(int sig){
    int estado;
    pid_t pid;
    int i;

    while ((pid = waitpid(-1, &estado, WNOHANG)) > 0) {
        for (i = 0; i < MAX_PROCS; i++){
            if (hijos[i] == pid){
                hijos[i] = 0;
            }
        }
        decrementar_vivos();
    }
}

static void manejadora_sigterm(int sig){
    int estado;
    int i;

    for (i = 0; i < MAX_PROCS; i++){
        if (hijos[i] != 0){
            kill(hijos[i], SIGTERM);
        }
    } 

    while (waitpid(-1, &estado, 0) > 0){
        decrementar_vivos();
    }
    imprimir_evento('M', getpid());
    exit(0);
}

static void manejadora_sigint(int sig){
    int estado;
    int i;
    static const char fin[] = "Programa acabado correctamente\n";

    for (i = 0; i < MAX_PROCS; i++)
        if (hijos[i] != 0)
            kill(hijos[i], SIGTERM);

    while (waitpid(-1, &estado, 0) > 0){};

    write(1, fin, sizeof(fin) - 1);
    close(fd);
    unlink(FICHERO);               
    exit(0);
}

static void crear_filial(void){
    sigset_t bloqueo, anterior;
    int datos[2];
    int espacio_libre = 0;
    pid_t pid;
    int i;
    struct sigaction configurar_senal;

    sigemptyset(&bloqueo);
    sigaddset(&bloqueo, SIGINT);
    sigaddset(&bloqueo, SIGTERM);
    sigaddset(&bloqueo, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bloqueo, &anterior);

    bloquear();
    leer_datos(datos);
    if (datos[1] < datos[0]) {
        datos[1]++;
        escribir_datos(datos);
        espacio_libre = 1;
    }
    desbloquear();

    if (!espacio_libre) {                  
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return;
    }

    pid = fork();

    if (pid == -1) {         
        decrementar_vivos();
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return;
    }

    if (pid == 0) {
        /* ---- Codigo del hijo ---- */
        soy_padre = 0;
        for (i = 0; i < MAX_PROCS; i++){
            hijos[i] = 0;
        }   
        srand(getpid());               
        imprimir_evento('V', getpid());  

        close(fd);                       
        fd = open(FICHERO, O_RDWR);      

     
        configurar_senal.sa_handler = manejadora_sigterm;
        sigemptyset(&configurar_senal.sa_mask);
        sigaddset(&configurar_senal.sa_mask, SIGINT);
        sigaddset(&configurar_senal.sa_mask, SIGCHLD);
        configurar_senal.sa_flags = 0;
        sigaction(SIGTERM, &configurar_senal, NULL);

        configurar_senal.sa_handler = SIG_IGN;
        sigemptyset(&configurar_senal.sa_mask);
        configurar_senal.sa_flags = 0;
        sigaction(SIGINT, &configurar_senal, NULL);

        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return;
    }

    for (i = 0; i < MAX_PROCS; i++) {
        if (hijos[i] == 0) {
            hijos[i] = pid;
            break;
        }
    }
    sigprocmask(SIG_SETMASK, &anterior, NULL);
}

int main(int argc, char *argv[]){
    static const char uso[] = "Uso: crisis n_max_procs [normal|veloz]\n";
    static const char error_num[] = "n_max_procs debe ser un entero entre 3 y 34\n";
    static const char error_vel[] = "La velocidad debe ser normal o veloz\n";
    int max_procesos = 0;
    int veloz = 0;
    int i;
    int datos[2];
    struct sigaction configurar_senal;

    if (argc != 2 && argc != 3) {
        write(2, uso, sizeof(uso) - 1);
        exit(1);
    }

    if (argv[1][0] == '\0') {
        write(2, error_num, sizeof(error_num) - 1);
        exit(1);
    }
    for (i = 0; argv[1][i] != '\0'; i++) {
        if (argv[1][i] < '0' || argv[1][i] > '9' || max_procesos > MAX_PROCS) {
            write(2, error_num, sizeof(error_num) - 1);
            exit(1);
        }
        max_procesos = max_procesos * 10 + (argv[1][i] - '0');
    }
    if (max_procesos < 3 || max_procesos > MAX_PROCS) {
        write(2, error_num, sizeof(error_num) - 1);
        exit(1);
    }

    if (argc == 3) {
        if (iguales(argv[2], "veloz"))
            veloz = 1;
        else if (!iguales(argv[2], "normal")) {
            write(2, error_vel, sizeof(error_vel) - 1);
            exit(1);
        }
    }

    fd = open(FICHERO, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd == -1)
        exit(1);
    datos[0] = max_procesos;
    datos[1] = 1;                   
    escribir_datos(datos);

    for (i = 0; i < MAX_PROCS; i++){
        hijos[i] = 0;
    }

    srand(getpid());

    configurar_senal.sa_handler = manejadora_sigchld;
    sigemptyset(&configurar_senal.sa_mask);
    sigaddset(&configurar_senal.sa_mask, SIGINT);
    sigaddset(&configurar_senal.sa_mask, SIGTERM);
    configurar_senal.sa_flags = 0;
    sigaction(SIGCHLD, &configurar_senal, NULL);

    configurar_senal.sa_handler = manejadora_sigint;
    sigemptyset(&configurar_senal.sa_mask);
    sigaddset(&configurar_senal.sa_mask, SIGCHLD);
    sigaddset(&configurar_senal.sa_mask, SIGTERM);
    configurar_senal.sa_flags = 0;
    sigaction(SIGINT, &configurar_senal, NULL);

    for (;;) {
        if (!veloz)
            dormir(1 + rand() % 4);

        if (soy_padre) {
            crear_filial();
        } else {
            if (rand() % 2 == 0)
                raise(SIGTERM);    
            else
                crear_filial();
        }
    }

    return 0;
}
// server.c - Servidor Metro Autónomo Telemetría (Linux/POSIX)
// Compilar: gcc server.c -o server -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define NUM_ESTACIONES 5
#define VUELTA_MAX 5
#define BATTERY_DECAY 5
#define BATTERY_RECHARGE 2
#define VEL_MAX 5
#define VEL_MIN 1

typedef struct {
    int sock;
    struct sockaddr_in addr;
    int isAdmin;
    char username[50];
} Client;

Client *clients[MAX_CLIENTS];
int num_clients = 0;
pthread_mutex_t clients_lock;

FILE *logFile;
int estacion = 1;
int direccion = 1; // 1 = adelante, -1 = atrás
int velocidad = 1; 
int bateria = 100;
int detenido = 0;
int estaciones_recorridas = 0;
char last_command[64] = "";

// ---------- Estado admin ----------
char admin_active = 0;
Client *admin_client = NULL;

// ---------- Logging ----------
void log_event_client(Client *c, const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s:%d -> %s\n",
             t->tm_hour, t->tm_min, t->tm_sec,
             inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), msg);
    printf("%s", buffer);
    if (logFile) {
        fprintf(logFile, "%s", buffer);
        fflush(logFile);
    }
}

void log_event(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s\n",
             t->tm_hour, t->tm_min, t->tm_sec, msg);
    printf("%s", buffer);
    if (logFile) {
        fprintf(logFile, "%s", buffer);
        fflush(logFile);
    }
}

// ---------- Autenticación ----------
int check_admin_password(const char *pass) {
    FILE *f = fopen("admin_pass.txt", "r");
    if (!f) return 0;
    char stored[100];
    if (!fgets(stored, sizeof(stored), f)) { fclose(f); return 0; }
    stored[strcspn(stored, "\n")] = 0;
    fclose(f);
    return strcmp(stored, pass) == 0;
}

// ---------- Enviar mensaje ----------
int send_to_client(Client *c, const char *msg) {
    if (c && c->sock >= 0) {
        int sent = send(c->sock, msg, strlen(msg), 0);
        if (sent < 0) return 0;
    }
    return 1;
}

// ---------- Eliminar cliente ----------
void remove_client(Client *c) {
    close(c->sock);
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < num_clients; i++) {
        if (clients[i] == c) {
            clients[i] = clients[num_clients - 1];
            num_clients--;
            break;
        }
    }
    if (c == admin_client) {
        admin_active = 0;
        admin_client = NULL;
        log_event("Administrador desconectado.");
    }
    pthread_mutex_unlock(&clients_lock);
    free(c);
}

// ---------- Telemetría individual ----------
void send_telemetry(Client *c) {
    char msg[512];
    char eventos[256] = "";

    if(bateria == 0) strcat(eventos, "DETENIDO_BATERIA;");
    if(estaciones_recorridas == 0) strcat(eventos, "CAMBIO_SENTIDO;");

    if(c->isAdmin){
        if(detenido && bateria>0) strcat(eventos, "DETENIDO_MANUAL;");
        if(estacion == NUM_ESTACIONES || estacion == 1) strcat(eventos, "PARADA_ESTACION;");
        if(strlen(last_command) > 0){
            strcat(eventos, last_command);
            last_command[0] = '\0';
        }
    }

    if(c->isAdmin){
        snprintf(msg, sizeof(msg),
              "TELEMETRIA Estacion:%d Direccion:%s Vel:%d Bateria:%d Estado:%s Rol:ADMIN Eventos:%s\n",
              estacion, direccion == 1 ? "IDA" : "VUELTA", velocidad, bateria,
              detenido ? "DETENIDO" : "MOVIMIENTO",
              eventos);
    } else {
        snprintf(msg, sizeof(msg),
              "TELEMETRIA Estacion:%d Direccion:%s Vel:%d Bateria:%d Estado:%s Rol:OBSERVER\n",
              estacion, direccion == 1 ? "IDA" : "VUELTA", velocidad, bateria,
              detenido ? "DETENIDO" : "MOVIMIENTO");
    }

    send_to_client(c, msg);
}

// ---------- Manejo de comandos ----------
void handle_command(Client *c, const char *cmd) {
    char logmsg[256];

    char command[BUFFER_SIZE];
    strncpy(command, cmd, BUFFER_SIZE);
    for(int i=0; command[i]; i++) command[i] = toupper(command[i]);

    if (!c->isAdmin) {
        send_to_client(c, "ERROR: Solo administrador puede enviar comandos.\n");
        return;
    }

    if (strncmp(command, "SPEEDUP", 7) == 0) {
        if (velocidad < VEL_MAX) { velocidad++; snprintf(logmsg, sizeof(logmsg), "CMD SPEEDUP -> Vel=%d", velocidad); strncpy(last_command, "CMD:SPEEDUP;", sizeof(last_command)); }
        else { send_to_client(c, "ERROR: Velocidad máxima alcanzada.\n"); return; }
    } else if (strncmp(command, "SLOWDOWN", 8) == 0) {
        if (velocidad > VEL_MIN) { velocidad--; snprintf(logmsg, sizeof(logmsg), "CMD SLOWDOWN -> Vel=%d", velocidad); strncpy(last_command, "CMD:SLOWDOWN;", sizeof(last_command)); }
        else { send_to_client(c, "ERROR: Velocidad mínima alcanzada.\n"); return; }
    } else if (strncmp(command, "STOPNOW", 7) == 0) {
        detenido = 1; snprintf(logmsg, sizeof(logmsg), "CMD STOPNOW -> Detenido"); strncpy(last_command, "CMD:STOPNOW;", sizeof(last_command));
    } else if (strncmp(command, "STARTNOW", 8) == 0) {
        if(bateria==0){ send_to_client(c, "ERROR: Batería agotada, no puede iniciar.\n"); return; }
        detenido = 0; snprintf(logmsg, sizeof(logmsg), "CMD STARTNOW -> Reanudado"); strncpy(last_command, "CMD:STARTNOW;", sizeof(last_command));
    } else {
        send_to_client(c, "ERROR: Comando invalido.\n");
        return;
    }

    log_event_client(c, logmsg);
    send_to_client(c, "OK\n");
}

// ---------- Hilo cliente ----------
void *client_thread(void *arg) {
    Client *c = (Client *)arg;
    char buffer[BUFFER_SIZE];
    int bytes;

    snprintf(buffer, sizeof(buffer), "Cliente conectado desde %s:%d",
             inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
    log_event(buffer);

    send_to_client(c, "Bienvenido al Metro.\nIngrese 'admin <password>' o 'observer'.\n");

    bytes = recv(c->sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes <= 0) { remove_client(c); return NULL; }
    buffer[bytes] = '\0';

    if (strncmp(buffer, "admin", 5) == 0) {
        char *pass = buffer + 6;
        pass[strcspn(pass, "\r\n")] = 0;
        if (check_admin_password(pass)) {
            if (admin_active && admin_client) { send_to_client(admin_client, "Otro admin conectado.\n"); remove_client(admin_client); }
            c->isAdmin = 1; admin_active = 1; admin_client = c;
            send_to_client(c, "Login Admin OK.\n");
            log_event_client(c, "Administrador autenticado.");
        } else { send_to_client(c, "Clave incorrecta.\n"); remove_client(c); return NULL; }
    } else if (strncmp(buffer, "observer", 8) == 0) {
        c->isAdmin = 0;
        send_to_client(c, "Modo observador.\n");
        log_event_client(c, "Observador autenticado.");
    } else { send_to_client(c, "ERROR: Rol invalido.\n"); remove_client(c); return NULL; }

    while ((bytes = recv(c->sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;
        handle_command(c, buffer);
    }

    log_event_client(c, "Cliente desconectado.");
    remove_client(c);
    return NULL;
}

// ---------- Hilo simulación metro ----------
void *metro_thread(void *arg) {
    int viaje_ticks = 0;
    int esperando = 0;

    while(1) {
        sleep(1); // tick cada segundo

        if(detenido) {
            bateria += BATTERY_RECHARGE;
            if(bateria > 100) bateria = 100;
            continue;
        }

        if(!esperando) {
            char msg[128];
            sprintf(msg, "en estación %d", estacion);
            log_event(msg);
            sprintf(msg, "espera de 20s en estación %d", estacion);
            log_event(msg);
            esperando = 1;
            viaje_ticks = 0;
            continue;
        }

        if(esperando && viaje_ticks < 20) {
            viaje_ticks++;
            continue;
        }

        if(esperando && viaje_ticks == 20) {
            int siguiente = estacion + direccion;
            char msg[128];
            sprintf(msg, "viajando de %d a %d", estacion, siguiente);
            log_event(msg);

            sleep(10);

            bateria -= 5;
            if(bateria < 0) bateria = 0;

            if(bateria == 0) {
                log_event("[BATERIA AGOTADA] Metro detenido 10s para recargar.");
                sleep(10);
                bateria = 20;
                log_event("[BATERIA RECARGADA] Metro reanuda recorrido.");
            }

            estacion = siguiente;
            estaciones_recorridas++;
            esperando = 0;

            if(estacion == NUM_ESTACIONES || estacion == 1) {
                log_event("[cambio de direccion]");
                direccion *= -1;
            }
        }
    }
    return NULL;
}

// ---------- Hilo telemetría ----------
void *telemetry_thread(void *arg) {
    while (1) {
        sleep(10);
        pthread_mutex_lock(&clients_lock);
        for(int i = 0; i < num_clients; i++){
            send_telemetry(clients[i]);
        }
        pthread_mutex_unlock(&clients_lock);
    }
    return NULL;
}

// ---------- Main ----------
int main(int argc, char *argv[]) {
    if (argc != 3) { printf("Uso: %s <puerto> <archivo_log>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);
    logFile = fopen(argv[2], "a");

    pthread_mutex_init(&clients_lock, NULL);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr; 
    addr.sin_family=AF_INET; 
    addr.sin_port=htons(port); 
    addr.sin_addr.s_addr=INADDR_ANY;

    if (bind(server,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("Error bind"); return 1; }
    listen(server, 5);

    log_event("Servidor iniciado.");

    pthread_t t1, t2;
    pthread_create(&t1,NULL,metro_thread,NULL);
    pthread_create(&t2,NULL,telemetry_thread,NULL);

    while(1){
        struct sockaddr_in cliaddr; socklen_t len=sizeof(cliaddr);
        int client_sock = accept(server,(struct sockaddr*)&cliaddr,&len);
        Client *c = (Client*)malloc(sizeof(Client));
        if(!c){ close(client_sock); continue; }
        c->sock = client_sock; c->addr = cliaddr; c->isAdmin = 0;

        pthread_mutex_lock(&clients_lock);
        if(num_clients<MAX_CLIENTS){ 
            clients[num_clients++] = c; 
            pthread_t tid; pthread_create(&tid,NULL,client_thread,c);
            pthread_detach(tid);
        }
        else { send(client_sock,"Servidor lleno.\n",16,0); close(client_sock); free(c); }
        pthread_mutex_unlock(&clients_lock);
    }

    close(server);
    if(logFile) fclose(logFile);
    pthread_mutex_destroy(&clients_lock);
    return 0;
}

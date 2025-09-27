// server.c - Servidor Metro Autónomo Telemetría (Linux)
// Compilar: gcc server.c -o server -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define NUM_ESTACIONES 5
#define VUELTA_MAX 5
#define BATTERY_DECAY 5
#define BATTERY_RECHARGE 2
#define VEL_MAX 5
#define VEL_MIN 1
#define DISTANCIA_ENTRE_ESTACIONES 10

typedef struct {
    int sock;
    struct sockaddr_in addr;
    int isAdmin;
    char username[50];
} Client;

Client *clients[MAX_CLIENTS];
int num_clients = 0;
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

FILE *logFile;
FILE *log_clients;
FILE *log_telemetry;
FILE *log_simulation;

int estacion = 1;
int direccion = 1;
int velocidad = 1;
int bateria = 100;
int detenido = 0;
int estaciones_recorridas = 0;
char last_command[64] = "";

int admin_active = 0;
Client *admin_client = NULL;

// ---------- Prototipos ----------
int send_to_client(Client *c, const char *msg);

// ---------- Logging ----------
void log_event_client(Client *c, const char *msg) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s (IP:%s) -> %s\n",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             c->username, inet_ntoa(c->addr.sin_addr), msg);
    printf("%s", buffer);
    if (logFile) fprintf(logFile, "%s", buffer);
    if (log_clients) fprintf(log_clients, "%s", buffer);
    fflush(logFile); fflush(log_clients);
}

void log_event(const char *msg) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s\n",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, msg);
    printf("%s", buffer);
    if (logFile) fprintf(logFile, "%s", buffer);
    if (log_simulation) fprintf(log_simulation, "%s", buffer);
    fflush(logFile); fflush(log_simulation);

    pthread_mutex_lock(&clients_lock);
    for(int i=0;i<num_clients;i++){
        send_to_client(clients[i], buffer);
    }
    pthread_mutex_unlock(&clients_lock);
}

// ---------- Autenticación ----------
int check_credentials(const char *user, const char *pass, const char *role_expected) {
    FILE *f = fopen("users.txt", "r");
    if (!f) return 0;
    char u[50], p[50], r[20];
    while (fscanf(f,"%s %s %s", u,p,r) == 3) {
        if (strcmp(u,user)==0 && strcmp(p,pass)==0 && strcmp(r,role_expected)==0){
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

// ---------- Enviar mensaje ----------
int send_to_client(Client *c, const char *msg){
    if(c && c->sock>=0){
        int sent = send(c->sock, msg, strlen(msg), 0);
        if(sent < 0) return 0;
    }
    return 1;
}

int username_in_use(const char *user, Client *self){
    pthread_mutex_lock(&clients_lock);
    for(int i=0;i<num_clients;i++){
        if(clients[i]!=self && strcmp(clients[i]->username,user)==0){
            pthread_mutex_unlock(&clients_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    return 0;
}

// ---------- Eliminar cliente ----------
void remove_client(Client *c){
    close(c->sock);
    pthread_mutex_lock(&clients_lock);
    for(int i=0;i<num_clients;i++){
        if(clients[i]==c){
            clients[i]=clients[num_clients-1];
            num_clients--;
            break;
        }
    }
    if(c==admin_client){
        admin_active=0;
        admin_client=NULL;
    }
    log_event_client(c,"Cliente desconectado.");
    pthread_mutex_unlock(&clients_lock);
    free(c);
}

void log_event_telemetry(const char *msg){
    time_t t=time(NULL);
    struct tm *tm_info=localtime(&t);
    char buffer[512];
    snprintf(buffer,sizeof(buffer),"[%02d:%02d:%02d] %s\n",
             tm_info->tm_hour,tm_info->tm_min,tm_info->tm_sec,msg);
    printf("%s",buffer);
    if(logFile) fprintf(logFile,"%s",buffer);
    if(log_telemetry) fprintf(log_telemetry,"%s",buffer);
    fflush(logFile); fflush(log_telemetry);
}

void send_telemetry(Client *c){
    char msg[512];
    if(c->isAdmin){
        snprintf(msg,sizeof(msg),"TELEMETRIA Estacion:%d Direccion:%s Vel:%d Bateria:%d Estado:%s Comando:%s\n",
                 estacion,direccion==1?"IDA":"VUELTA",velocidad,bateria,detenido?"DETENIDO":"MOVIMIENTO",last_command);
        last_command[0]='\0';
    } else {
        snprintf(msg,sizeof(msg),"TELEMETRIA Estacion:%d Direccion:%s Vel:%d Bateria:%d Estado:%s\n",
                 estacion,direccion==1?"IDA":"VUELTA",velocidad,bateria,detenido?"DETENIDO":"MOVIMIENTO");
    }
    printf("%s",msg);
    log_event_telemetry(msg);
    send_to_client(c,msg);
}

// ---------- Manejo de comandos ----------
void handle_command(Client *c,const char *cmd){
    char logmsg[256];
    char command[BUFFER_SIZE];
    strncpy(command,cmd,BUFFER_SIZE);
    for(int i=0;command[i];i++) command[i]=toupper(command[i]);

    if(!c->isAdmin){
        send_to_client(c,"ERROR: Solo administrador puede enviar comandos.\n");
        return;
    }

    if(strncmp(command,"SPEEDUP",7)==0){
        if(velocidad<VEL_MAX){velocidad++; snprintf(logmsg,sizeof(logmsg),"CMD SPEEDUP -> Vel=%d",velocidad); strncpy(last_command,"CMD:SPEEDUP;",sizeof(last_command));}
        else{send_to_client(c,"ERROR: Velocidad máxima alcanzada.\n");return;}
    } else if(strncmp(command,"SLOWDOWN",8)==0){
        if(velocidad>VEL_MIN){velocidad--; snprintf(logmsg,sizeof(logmsg),"CMD SLOWDOWN -> Vel=%d",velocidad); strncpy(last_command,"CMD:SLOWDOWN;",sizeof(last_command));}
        else{send_to_client(c,"ERROR: Velocidad mínima alcanzada.\n");return;}
    } else if(strncmp(command,"STOPNOW",7)==0){
        detenido=1; snprintf(logmsg,sizeof(logmsg),"CMD STOPNOW -> Detenido"); strncpy(last_command,"CMD:STOPNOW;",sizeof(last_command));
    } else if(strncmp(command,"STARTNOW",8)==0){
        if(bateria==0){ send_to_client(c,"ERROR: Batería agotada, no puede iniciar.\n"); return;}
        detenido=0; snprintf(logmsg,sizeof(logmsg),"CMD STARTNOW -> Reanudado"); strncpy(last_command,"CMD:STARTNOW;",sizeof(last_command));
    } else {
        send_to_client(c,"ERROR: Comando invalido.\n"); return;
    }
    log_event_client(c,logmsg);
    send_to_client(c,"OK\n");
}

// ---------- Hilos ----------
void *client_thread(void *arg){
    Client *c=(Client*)arg;
    char buffer[BUFFER_SIZE];
    snprintf(buffer,sizeof(buffer),"Cliente intentando conectarse desde %s:%d", inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
    log_event(buffer);

    send_to_client(c,"Bienvenido al Metro.\nIngrese 'admin <usuario> <password>' o 'observer'.\n");

    int bytes = recv(c->sock,buffer,BUFFER_SIZE-1,0);
    if(bytes<=0){remove_client(c);return NULL;}
    buffer[bytes]='\0';

    char cmd[16],user[50],pass[50];
    if(sscanf(buffer,"%15s %49s %49s",cmd,user,pass)>=1){
        if(strcmp(cmd,"admin")==0){
            if(check_credentials(user,pass,"admin")){
                strcpy(c->username,user);
                if(username_in_use(c->username,c)){
                    send_to_client(c,"ERROR: Ese nombre ya está en uso.\n"); remove_client(c); return NULL;
                }
                c->isAdmin=1; admin_active=1; admin_client=c;
                send_to_client(c,"Login Admin OK.\n");
                log_event_client(c,"Administrador autenticado.");
            } else{
                send_to_client(c,"Credenciales admin incorrectas.\n");
                log_event_client(c,"Intento de login admin fallido.");
                remove_client(c); return NULL;
            }
        } else if(strcmp(cmd,"observer")==0){
            c->isAdmin=0;
            if(strlen(user)>0) strncpy(c->username,user,sizeof(c->username)); else strcpy(c->username,"Observer");
            if(username_in_use(c->username,c)){ send_to_client(c,"ERROR: Ese nombre ya está en uso.\n"); remove_client(c); return NULL; }
            send_to_client(c,"Modo observador.\n");
            log_event_client(c,"Observador autenticado.");
        } else{
            send_to_client(c,"ERROR: Rol invalido.\n"); remove_client(c); return NULL;
        }
    }

    char conn_msg[128];
    snprintf(conn_msg,sizeof(conn_msg),"Cliente conectado: %s (IP: %s) Rol: %s",
             c->username, inet_ntoa(c->addr.sin_addr), c->isAdmin?"ADMIN":"OBSERVER");
    log_event(conn_msg);

    while((bytes=recv(c->sock,buffer,BUFFER_SIZE-1,0))>0){
        buffer[bytes]='\0';
        buffer[strcspn(buffer,"\r\n")]=0;
        handle_command(c,buffer);
    }
    remove_client(c);
    return NULL;
}

void *metro_thread(void *arg){
    int esperando=0, wait_ticks=0, progreso=0;
    while(1){
        sleep(1);
        if(detenido){
            bateria+=BATTERY_RECHARGE;
            if(bateria>100) bateria=100;
            continue;
        }
        if(!esperando){
            char msg[128]; sprintf(msg,"en estación %d",estacion); log_event(msg);
            sprintf(msg,"espera de 20s en estación %d",estacion); log_event(msg);
            esperando=1; wait_ticks=0; progreso=0; continue;
        }
        if(esperando && wait_ticks<20){ wait_ticks++; continue;}
        if(esperando && wait_ticks==20){
            int siguiente=estacion+direccion;
            if(siguiente<1) siguiente=1; if(siguiente>NUM_ESTACIONES)siguiente=NUM_ESTACIONES;
            char msg[128]; sprintf(msg,"viajando de %d a %d a vel %d",estacion,siguiente,velocidad); log_event(msg);
            while(progreso<DISTANCIA_ENTRE_ESTACIONES){
                sleep(1);
                if(detenido) break;
                int v=(velocidad>0?velocidad:1);
                progreso+=v;
                if(progreso>DISTANCIA_ENTRE_ESTACIONES) progreso=DISTANCIA_ENTRE_ESTACIONES;
            }
            if(detenido){ log_event("[DETENIDO EN VIA] Esperando STARTNOW..."); continue;}
            estacion=siguiente; estaciones_recorridas++; esperando=0; progreso=0;
            bateria-=BATTERY_DECAY; if(bateria<0)bateria=0;
            if(bateria==0){ log_event("[BATERIA AGOTADA] Metro detenido 10s para recargar."); sleep(10); bateria=20; log_event("[BATERIA RECARGADA] Metro reanuda recorrido.");}
            if(estacion==NUM_ESTACIONES || estacion==1){ log_event("[cambio de direccion]"); direccion*=-1;}
        }
    }
    return NULL;
}

void *telemetry_thread(void *arg){
    while(1){
        sleep(10);
        pthread_mutex_lock(&clients_lock);
        for(int i=0;i<num_clients;i++){
            send_telemetry(clients[i]);
        }
        pthread_mutex_unlock(&clients_lock);
    }
    return NULL;
}

// ---------- Main ----------
int main(int argc,char *argv[]){
    if(argc!=3){ printf("Uso: %s <puerto> <archivo_log>\n",argv[0]); return 1;}
    int port=atoi(argv[1]);
    logFile=fopen(argv[2],"a");
    log_clients=fopen("clientes.log","a");
    log_telemetry=fopen("telemetria.log","a");
    log_simulation=fopen("simulacion.log","a");
    if(!log_clients||!log_telemetry||!log_simulation){printf("Error abriendo archivos de log.\n"); return 1;}

    int server=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in addr; addr.sin_family=AF_INET; addr.sin_port=htons(port); addr.sin_addr.s_addr=INADDR_ANY;
    if(bind(server,(struct sockaddr*)&addr,sizeof(addr))<0){printf("Error bind\n"); return 1;}
    if(listen(server,5)<0){printf("Error listen\n"); return 1;}

    log_event("Servidor iniciado.");

    pthread_t metro_tid, telem_tid;
    pthread_create(&metro_tid, NULL, metro_thread, NULL);
    pthread_create(&telem_tid, NULL, telemetry_thread, NULL);

    while(1){
        struct sockaddr_in cliaddr; socklen_t len=sizeof(cliaddr);
        int client_sock=accept(server,(struct sockaddr*)&cliaddr,&len);
        if(client_sock<0) continue;

        Client *c=(Client*)malloc(sizeof(Client));
        if(!c){ close(client_sock); continue; }
        c->sock=client_sock; c->addr=cliaddr; c->isAdmin=0;

        pthread_mutex_lock(&clients_lock);
        if(num_clients<MAX_CLIENTS){ clients[num_clients++]=c; pthread_t tid; pthread_create(&tid,NULL,client_thread,c);}
        else{ send(client_sock,"Servidor lleno.\n",16,0); close(client_sock); free(c);}
        pthread_mutex_unlock(&clients_lock);
    }

    close(server);
    if(logFile) fclose(logFile);
    if(log_clients) fclose(log_clients);
    if(log_telemetry) fclose(log_telemetry);
    if(log_simulation) fclose(log_simulation);

    pthread_mutex_destroy(&clients_lock);
    return 0;
}
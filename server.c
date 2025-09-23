// server.c - Servidor Metro Autónomo Telemetría (Windows)
// Compilar: gcc server.c -o server.exe -lws2_32

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define MAX_CLIENTS 10
#define BUFFER_SIZE 1024
#define NUM_ESTACIONES 5
#define VUELTA_MAX 5
#define BATTERY_DECAY 5
#define BATTERY_RECHARGE 2
#define VEL_MAX 5
#define VEL_MIN 1

typedef struct {
    SOCKET sock;
    struct sockaddr_in addr;
    int isAdmin;
    char username[50];
} Client;

Client *clients[MAX_CLIENTS];
int num_clients = 0;
CRITICAL_SECTION clients_lock;

FILE *logFile;
int estacion = 1;
int direccion = 1; // 1 = adelante, -1 = atrás
int velocidad = 1; 
int bateria = 100;
int detenido = 0;
int estaciones_recorridas = 0;
int parada_ticks = 0;
char last_command[64] = "";

// ---------- Estado admin ----------
char admin_active = 0;
Client *admin_client = NULL;

// ---------- Logging ----------
void log_event_client(Client *c, const char *msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buffer[512];
    _snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s:%d -> %s\n",
              t.wHour, t.wMinute, t.wSecond,
              inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port), msg);
    printf("%s", buffer);
    if (logFile) {
        fprintf(logFile, "%s", buffer);
        fflush(logFile);
    }
}

void log_event(const char *msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buffer[512];
    _snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] %s\n",
              t.wHour, t.wMinute, t.wSecond, msg);
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
    if (c && c->sock != INVALID_SOCKET) {
        int sent = send(c->sock, msg, (int)strlen(msg), 0);
        if (sent == SOCKET_ERROR) return 0;
    }
    return 1;
}

// ---------- Eliminar cliente ----------
void remove_client(Client *c) {
    closesocket(c->sock);
    EnterCriticalSection(&clients_lock);
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
    LeaveCriticalSection(&clients_lock);
    free(c);
}

// ---------- Telemetría individual ----------
void send_telemetry(Client *c) {
    char msg[512];
    char eventos[256] = "";

    // Eventos importantes comunes
    if(bateria == 0) strcat(eventos, "DETENIDO_BATERIA;");
    if(estaciones_recorridas == 0) strcat(eventos, "CAMBIO_SENTIDO;");

    // Eventos adicionales para Admin
    if(c->isAdmin){
        if(detenido && bateria>0) strcat(eventos, "DETENIDO_MANUAL;");
        if(estacion == NUM_ESTACIONES || estacion == 1) strcat(eventos, "PARADA_ESTACION;");
        if(strlen(last_command) > 0){
            strcat(eventos, last_command);
            last_command[0] = '\0';
        }
    }

    // Telemetría diferenciada
    if(c->isAdmin){
        _snprintf(msg, sizeof(msg),
              "TELEMETRIA Estacion:%d Direccion:%s Vel:%d Bateria:%d Estado:%s Rol:ADMIN Eventos:%s\n",
              estacion, direccion == 1 ? "IDA" : "VUELTA", velocidad, bateria,
              detenido ? "DETENIDO" : "MOVIMIENTO",
              eventos);
    } else {
        _snprintf(msg, sizeof(msg),
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
DWORD WINAPI client_thread(LPVOID arg) {
    Client *c = (Client *)arg;
    char buffer[BUFFER_SIZE];
    int bytes;

    snprintf(buffer, sizeof(buffer), "Cliente conectado desde %s:%d",
             inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
    log_event(buffer);

    send_to_client(c, "Bienvenido al Metro.\nIngrese 'admin <password>' o 'observer'.\n");

    bytes = recv(c->sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes <= 0) { remove_client(c); return 0; }
    buffer[bytes] = '\0';

    if (strncmp(buffer, "admin", 5) == 0) {
        char *pass = buffer + 6;
        pass[strcspn(pass, "\r\n")] = 0;
        if (check_admin_password(pass)) {
            if (admin_active && admin_client) { send_to_client(admin_client, "Otro admin conectado.\n"); remove_client(admin_client); }
            c->isAdmin = 1; admin_active = 1; admin_client = c;
            send_to_client(c, "Login Admin OK.\n");
            log_event_client(c, "Administrador autenticado.");
        } else { send_to_client(c, "Clave incorrecta.\n"); remove_client(c); return 0; }
    } else if (strncmp(buffer, "observer", 8) == 0) {
        c->isAdmin = 0;
        send_to_client(c, "Modo observador.\n");
        log_event_client(c, "Observador autenticado.");
    } else { send_to_client(c, "ERROR: Rol invalido.\n"); remove_client(c); return 0; }

    while ((bytes = recv(c->sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;
        handle_command(c, buffer);
    }

    log_event_client(c, "Cliente desconectado.");
    remove_client(c);
    return 0;
}

// ---------- Hilo simulación metro ----------
DWORD WINAPI metro_thread(LPVOID arg) {
    int viaje_ticks = 0;
    int esperando = 0;

    while(1) {
        Sleep(1000); // tick cada segundo

        if(detenido) {
            bateria += BATTERY_RECHARGE;
            if(bateria > 100) bateria = 100;
            continue;
        }

        // Inicio de espera
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

        // Espera de 20s
        if(esperando && viaje_ticks < 20) {
            viaje_ticks++;
            continue;
        }

        // Fin de espera → viajar
        if(esperando && viaje_ticks == 20) {
            int siguiente = estacion + direccion;
            char msg[128];
            sprintf(msg, "viajando de %d a %d", estacion, siguiente);
            log_event(msg);

            // Simula viaje de 10s
            /*for(int i=0;i<10;i++){
                Sleep(1000);
                // aquí podrías disminuir batería en tiempo real si querés
                bateria -= BATTERY_DECAY/10;
                if(bateria<0) bateria=0;
            }*/

            
            // Simula viaje de 10s
            Sleep(10000);

            // Al llegar a la estación, reducir batería 5%
            bateria -= 5;
            if(bateria < 0) bateria = 0;

            // Si batería se agotó → parar 10s para recargar
            if(bateria == 0) {
                log_event("[BATERIA AGOTADA] Metro detenido 10s para recargar.");
                Sleep(10000);
                bateria = 20; // recarga parcial, o puedes poner 100 si quieres full
                log_event("[BATERIA RECARGADA] Metro reanuda recorrido.");
            }

            estacion = siguiente;
            estaciones_recorridas++;
            esperando = 0;

            // Cambio de dirección al llegar a extremos
            if(estacion == NUM_ESTACIONES || estacion == 1) {
                log_event("[cambio de direccion]");
                direccion *= -1;
            }
        }
    }
    return 0;
}

// ---------- Hilo telemetría ----------
DWORD WINAPI telemetry_thread(LPVOID arg) {
    while (1) {
        Sleep(10000); // cada 10s
        EnterCriticalSection(&clients_lock);
        for(int i = 0; i < num_clients; i++){
            send_telemetry(clients[i]); // individual por cliente
        }
        LeaveCriticalSection(&clients_lock);
    }
    return 0;
}

// ---------- Main ----------
int main(int argc, char *argv[]) {
    if (argc != 3) { printf("Uso: %s <puerto> <archivo_log>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);
    logFile = fopen(argv[2], "a");

    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa)!=0){ printf("Error Winsock\n"); return 1; }
    InitializeCriticalSection(&clients_lock);

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr; addr.sin_family=AF_INET; addr.sin_port=htons(port); addr.sin_addr.s_addr=INADDR_ANY;
    if (bind(server,(struct sockaddr*)&addr,sizeof(addr))==SOCKET_ERROR){ printf("Error bind\n"); return 1; }
    listen(server, 5);

    log_event("Servidor iniciado.");

    CreateThread(NULL,0,metro_thread,NULL,0,NULL);
    CreateThread(NULL,0,telemetry_thread,NULL,0,NULL);

    while(1){
        struct sockaddr_in cliaddr; int len=sizeof(cliaddr);
        SOCKET client_sock = accept(server,(struct sockaddr*)&cliaddr,&len);
        Client *c = (Client*)malloc(sizeof(Client));
        if(!c){ closesocket(client_sock); continue; }
        c->sock = client_sock; c->addr = cliaddr; c->isAdmin = 0;

        EnterCriticalSection(&clients_lock);
        if(num_clients<MAX_CLIENTS){ clients[num_clients++] = c; CreateThread(NULL,0,client_thread,c,0,NULL); }
        else { send(client_sock,"Servidor lleno.\n",16,0); closesocket(client_sock); free(c); }
        LeaveCriticalSection(&clients_lock);
    }

    closesocket(server);
    if(logFile) fclose(logFile);
    DeleteCriticalSection(&clients_lock);
    WSACleanup();
    return 0;
}

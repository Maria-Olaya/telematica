// client_linux.c - Cliente Metro Autónomo (Linux) - robusto para login
// Compilar: gcc client_linux.c -o client_linux -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>

#define BUFFER_SIZE 4096

static int g_sock = -1;
static int g_is_admin = 0;

// Render simple de telemetría
static void render_telemetry(const char *line) {
    if (strncmp(line, "TELEMETRIA ", 11) != 0) {
        printf("%s\n", line);
        return;
    }
    printf("%s\n", line);
}

// Hilo receptor: imprime todo lo que llegue (después del login)
void *recv_thread(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];
    int bytes;

    while ((bytes = recv(g_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes] = '\0';

        char *p = buffer;
        while (p && *p) {
            char *nl = strpbrk(p, "\r\n");
            if (nl) *nl = '\0';
            if (*p) {
                if (strncmp(p, "TELEMETRIA ", 11) == 0) render_telemetry(p);
                else printf("%s\n", p);
            }
            if (!nl) break;
            p = nl + 1;
            while (*p == '\r' || *p == '\n') p++;
        }
    }

    printf("\nConexión cerrada por el servidor.\n");
    exit(0);
    return NULL;
}

// Recibe la bienvenida inicial (una vez)
static void recv_initial_prompt(void) {
    char buf[BUFFER_SIZE];
    int bytes = recv(g_sock, buf, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buf[bytes] = '\0';
        printf("%s", buf);
    }
}

// Espera hasta ver el mensaje de autenticación (ignorando telemetría)
static int wait_for_auth(int s) {
    char buf[BUFFER_SIZE];
    for (;;) {
        int r = recv(s, buf, sizeof(buf)-1, 0);
        if (r <= 0) return -1; // cerrado/error
        buf[r] = '\0';

        char *p = buf;
        while (p && *p) {
            char *nl = strpbrk(p, "\r\n");
            if (nl) *nl = '\0';

            if (*p) {
                if (strstr(p, "Login Admin OK.") != NULL) {
                    printf("%s\n", p);
                    return 1;
                }
                if (strstr(p, "Modo observador.") != NULL) {
                    printf("%s\n", p);
                    return 0;
                }
                if (strstr(p, "Clave incorrecta.") != NULL ||
                    strncmp(p, "ERROR:", 6) == 0) {
                    printf("%s\n", p);
                    return -1;
                }
            }
            if (!nl) break;
            p = nl + 1;
            while (*p == '\r' || *p == '\n') p++;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <ip_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        perror("Error al crear socket");
        return 1;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &server.sin_addr) <= 0) {
        perror("IP inválida");
        close(g_sock);
        return 1;
    }

    if (connect(g_sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        perror("Error al conectar con el servidor");
        close(g_sock);
        return 1;
    }

    printf("Conectado al servidor %s:%d\n", server_ip, port);

    // 1) Bienvenida
    recv_initial_prompt();

    // 2) Rol
    char input[BUFFER_SIZE];
    printf("\nElige rol: 'observer'  o  'admin <usuario> <password>'\n> ");
    if (!fgets(input, sizeof(input), stdin)) {
        close(g_sock);
        return 0;
    }
    send(g_sock, input, strlen(input), 0);

    // 3) Esperar veredicto
    int who = wait_for_auth(g_sock);

    if (who == -1) {
        printf("¡Credenciales incorrectas! Intenta de nuevo.\n");
    }

    if (who == 1) {
        g_is_admin = 1;
        printf("\nComandos: SPEEDUP | SLOWDOWN | STOPNOW | STARTNOW\n");
    } else if (who == 0) {
        g_is_admin = 0;
        printf("Recibirás telemetría periódica.\n");
    } else {
        close(g_sock);
        return 0;
    }


    

    // 4) Hilo receptor
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, NULL);

    // 5) Enviar comandos
    for (;;) {
        printf("> ");
        if (!fgets(input, sizeof(input), stdin)) break;
        if (send(g_sock, input, strlen(input), 0) < 0) {
            perror("Error al enviar datos");
            break;
        }
    }

    close(g_sock);
    return 0;
}


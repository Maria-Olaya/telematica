// client.c - Cliente Metro Autónomo (Linux)
// Compilar: gcc client.c -o client -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

void* recv_thread(void* arg) {
    int sock = *(int*)arg;
    char buffer[BUFFER_SIZE];
    int bytes;
    while ((bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
    }
    printf("Conexión cerrada por el servidor.\n");
    exit(0);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <ip_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1];
    int port = atoi(argv[2]);

    // Crear socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error al crear socket");
        return 1;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server.sin_addr) <= 0) {
        perror("Dirección inválida");
        close(sock);
        return 1;
    }

    // Conexión
    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        perror("Error al conectar con el servidor");
        close(sock);
        return 1;
    }

    printf("Conectado al servidor %s:%d\n", server_ip, port);

    // Hilo receptor
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, &sock);

    // Entrada usuario
    char input[BUFFER_SIZE];
    while (1) {
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        if (send(sock, input, strlen(input), 0) < 0) {
            perror("Error al enviar datos");
            break;
        }
    }

    close(sock);
    return 0;
}

// client.c - Cliente Metro Autónomo (Windows)
// Compilar: gcc client.c -o client.exe -lws2_32

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 1024

DWORD WINAPI recv_thread(LPVOID arg) {
    SOCKET sock = *(SOCKET*)arg;
    char buffer[BUFFER_SIZE];
    int bytes;
    while ((bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
    }
    printf("Conexión cerrada por el servidor.\n");
    exit(0);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Uso: %s <ip_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1];
    int port = atoi(argv[2]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("Error al inicializar Winsock.\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Error al crear socket.\n");
        WSACleanup();
        return 1;
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(server_ip);

    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        printf("Error al conectar con el servidor.\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("Conectado al servidor %s:%d\n", server_ip, port);

    // Hilo para recibir mensajes
    CreateThread(NULL, 0, recv_thread, &sock, 0, NULL);

    // Bucle de entrada de usuario
    char input[BUFFER_SIZE];
    while (1) {
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        if (send(sock, input, (int)strlen(input), 0) == SOCKET_ERROR) {
            printf("Error al enviar datos.\n");
            break;
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}

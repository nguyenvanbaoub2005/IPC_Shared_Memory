#include "../include/client_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8080

int main() {
    printf("=== IPC CHAT CLIENT (SOCKET MODE) ===\n");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return 1;
    }

    // Login
    char username[32], password[32], creds[64];
    printf("Username: "); scanf("%31s", username);
    printf("Password: "); scanf("%31s", password);
    getchar(); // bỏ newline

    snprintf(creds, sizeof(creds), "%s:%s", username, password);
    if (send(sock, creds, strlen(creds), 0) < 0) {
        printf("Failed to send login credentials: %s\n", strerror(errno));
        close(sock);
        return 1;
    }

    char response[128];
    int len = recv(sock, response, sizeof(response) - 1, 0);
    if (len <= 0) {
        printf("Server closed connection: %s\n", len == 0 ? "Server closed" : strerror(errno));
        close(sock);
        return 1;
    }
    response[len] = '\0';

    if (strstr(response, "failed")) {
        printf("❌ %s\n", response);
        close(sock);
        return 0;
    }

    printf("✅ %s\n", response);
    print_menu();

    handle_server_message(sock);
    handle_user_input(sock, username);

    return 0;
}
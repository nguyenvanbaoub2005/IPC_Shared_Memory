#include "../include/client_utils.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

void print_menu() {
    printf("\n=== COMMAND MENU ===\n");
    printf("/menu              : Show this menu\n");
    printf("/users             : List online users\n");
    printf("/groups            : List all groups\n");
    printf("|<username>        : View chat history with user\n");
    printf("|<groupId>         : View group chat history\n");
    printf("/<username> <msg>  : Send private message\n");
    printf("/<groupId> <msg>   : Send message to group\n");
    printf("/exit              : Logout\n");
    printf("====================\n");
}

void *recv_thread(void *arg) {
    int sock = *(int *)arg;
    char buffer[BUFFER_SIZE];
    char *full_message = NULL;
    size_t total_len = 0;
    int len;

    while ((len = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[len] = '\0';
        full_message = realloc(full_message, total_len + len + 1);
        memcpy(full_message + total_len, buffer, len);
        total_len += len;
        full_message[total_len] = '\0';

        if (strstr(full_message, "=== End of History ===\n") || total_len < BUFFER_SIZE - 1) {
            printf("%s", full_message);
            free(full_message);
            full_message = NULL;
            total_len = 0;
        }
        fflush(stdout);
    }

    if (full_message) {
        free(full_message);
    }
    printf("\n[Disconnected from server]: %s\n", len == 0 ? "Server closed connection" : strerror(errno));
    close(sock);
    exit(0);
}

void handle_server_message(int sock) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, &sock) != 0) {
        printf("Failed to create receive thread: %s\n", strerror(errno));
        close(sock);
        exit(1);
    }
    pthread_detach(tid);
}

void handle_user_input(int sock, const char *username) {
    char msg[BUFFER_SIZE];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(msg, sizeof(msg), stdin))
            break;
        msg[strcspn(msg, "\n")] = 0;

        // Cắt khoảng trắng đầu và cuối
        char *start = msg;
        while (*start == ' ') start++;
        char *end = start + strlen(start) - 1;
        while (end > start && *end == ' ') end--;
        *(end + 1) = '\0';

        if (strcmp(start, "/exit") == 0) {
            if (send(sock, "/exit", 5, 0) < 0) {
                printf("Failed to send /exit: %s\n", strerror(errno));
            }
            break;
        }

        if (strlen(start) == 0) continue;

        // Kiểm tra nếu là tin nhắn riêng hoặc nhóm (bắt đầu bằng '/')
        if (start[0] == '/' && strcmp(start, "/menu") != 0 && 
            strcmp(start, "/users") != 0 && strcmp(start, "/groups") != 0) {
            char target[32] = {0}, message[BUFFER_SIZE] = {0};
            char *space = strchr(start + 1, ' ');
            if (space) {
                strncpy(target, start + 1, space - (start + 1));
                target[space - (start + 1)] = '\0';
                strcpy(message, space + 1);
                printf("To %s: %s\n", target, message);
            } else {
                printf("Sending to server: %s\n", start);
            }
        } else {
            printf("Sending to server: %s\n", start);
        }

        if (send(sock, start, strlen(start), 0) < 0) {
            printf("Failed to send to server: %s\n", strerror(errno));
        }
    }
    close(sock);
}
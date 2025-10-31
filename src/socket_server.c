#include "../include/server_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define PORT 8080

typedef struct {
    int socket;
    char username[32];
} Client;

Client clients[MAX_CLIENTS];
int clientCount = 0;

// ========================= TIỆN ÍCH SERVER =========================
Client *find_client_by_name(const char *username) {
    for (int i = 0; i < clientCount; i++) {
        if (strcmp(clients[i].username, username) == 0)
            return &clients[i];
    }
    return NULL;
}

void broadcast(const char *sender, const char *msg) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "[%s -> ALL]: %s\n", sender, msg);
    for (int i = 0; i < clientCount; i++) {
        if (send(clients[i].socket, buffer, strlen(buffer), 0) < 0) {
            log_event("[ERROR] Failed to send broadcast to %s: %s", clients[i].username, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send broadcast to %s: %s\n", clients[i].username, strerror(errno));
        }
    }
    log_event("%s broadcast: %s", sender, msg);
}

void send_private(const char *sender, const char *target, const char *msg) {
    Client *receiver = find_client_by_name(target);
    char buffer[BUFFER_SIZE];
    if (receiver) {
        snprintf(buffer, sizeof(buffer), "[PM %s → %s]: %s\n", sender, target, msg);
        if (send(receiver->socket, buffer, strlen(buffer), 0) < 0) {
            log_event("[ERROR] Failed to send private message to %s: %s", target, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send private message to %s: %s\n", target, strerror(errno));
        }
        save_conversation(sender, target, msg, 0);
        log_event("%s → %s: %s", sender, target, msg);
    } else {
        snprintf(buffer, sizeof(buffer), "[Server] User %s not found.\n", target);
        Client *senderClient = find_client_by_name(sender);
        if (senderClient && send(senderClient->socket, buffer, strlen(buffer), 0) < 0) {
            log_event("[ERROR] Failed to send error message to %s: %s", sender, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send error message to %s: %s\n", sender, strerror(errno));
        }
    }
}

void send_group_message(const char *sender, const char *groupId, const char *msg) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "[%s@%s]: %s\n", sender, groupId, msg);
    for (int i = 0; i < clientCount; i++) {
        if (is_user_in_group(groupId, clients[i].username)) {
            if (send(clients[i].socket, buffer, strlen(buffer), 0) < 0) {
                log_event("[ERROR] Failed to send group message to %s: %s", clients[i].username, strerror(errno));
                fprintf(stderr, "[ERROR] Failed to send group message to %s: %s\n", clients[i].username, strerror(errno));
            }
        }
    }
    save_conversation(sender, groupId, msg, 1);
    log_event("%s → GROUP %s: %s", sender, groupId, msg);
}

void show_menu(int sock) {
    const char *menu =
        "\n=== COMMAND MENU ===\n"
        "/menu              : Show this menu\n"
        "/users             : List online users\n"
        "/groups            : List all groups\n"
        "|<username>        : View chat history with user\n"
        "|<groupId>         : View group chat history\n"
        "/<username> <msg>  : Send private message\n"
        "/<groupId> <msg>   : Send message to group\n"
        "/exit              : Logout\n";
    if (send(sock, menu, strlen(menu), 0) < 0) {
        log_event("[ERROR] Failed to send menu to socket %d: %s", sock, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to send menu to socket %d: %s\n", sock, strerror(errno));
    }
}

void show_users(int sock) {
    char buffer[BUFFER_SIZE] = "=== Online Users ===\n";
    for (int i = 0; i < clientCount; i++) {
        strcat(buffer, clients[i].username);
        strcat(buffer, "\n");
    }
    if (send(sock, buffer, strlen(buffer), 0) < 0) {
        log_event("[ERROR] Failed to send user list to socket %d: %s", sock, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to send user list to socket %d: %s\n", sock, strerror(errno));
    }
}

void show_groups_for_user(int sock, const char *username) {
    char buffer[BUFFER_SIZE] = "=== Your Groups ===\n";
    int found = 0;
    for (int i = 0; i < groupCount; i++) {
        if (is_user_in_group(groups[i].groupId, username)) {
            strcat(buffer, groups[i].groupId);
            strcat(buffer, " - ");
            strcat(buffer, groups[i].groupName);
            strcat(buffer, "\n");
            found = 1;
        }
    }
    if (!found)
        strcat(buffer, "(You are not in any groups)\n");
    if (send(sock, buffer, strlen(buffer), 0) < 0) {
        log_event("[ERROR] Failed to send group list to socket %d: %s", sock, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to send group list to socket %d: %s\n", sock, strerror(errno));
    }
}

int check_login(const char *username, const char *password) {
    for (int i = 0; i < userCount; i++) {
        if (strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password, password) == 0)
            return 1;
    }
    return 0;
}

void remove_client(int socket) {
    for (int i = 0; i < clientCount; i++) {
        if (clients[i].socket == socket) {
            log_event("%s disconnected", clients[i].username);
            shutdown(clients[i].socket, SHUT_RDWR);
            close(clients[i].socket);
            for (int j = i; j < clientCount - 1; j++)
                clients[j] = clients[j + 1];
            clientCount--;
            break;
        }
    }
}

// ========================= XỬ LÝ CLIENT =========================
void *client_handler(void *arg) {
    int sock = *(int *)arg;
    char buffer[BUFFER_SIZE], username[32], password[32];

    // Nhận thông tin đăng nhập
    int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (len <= 0) {
        log_event("[ERROR] Failed to receive login data for socket %d: %s", sock, len == 0 ? "Connection closed" : strerror(errno));
        fprintf(stderr, "[ERROR] Failed to receive login data for socket %d: %s\n", sock, len == 0 ? "Connection closed" : strerror(errno));
        close(sock);
        pthread_exit(NULL);
    }
    buffer[len] = '\0';
    sscanf(buffer, "%31[^:]:%31s", username, password);
    log_event("Login attempt: username=%s", username);

    if (!check_login(username, password)) {
        if (send(sock, "Login failed\n", 13, 0) < 0) {
            log_event("[ERROR] Failed to send login failed message to socket %d: %s", sock, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send login failed message to socket %d: %s\n", sock, strerror(errno));
        }
        close(sock);
        pthread_exit(NULL);
    }

    // Kiểm tra trùng username
    if (find_client_by_name(username)) {
        if (send(sock, "Login failed: Username already in use\n", 37, 0) < 0) {
            log_event("[ERROR] Failed to send duplicate username message to socket %d: %s", sock, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send duplicate username message to socket %d: %s\n", sock, strerror(errno));
        }
        close(sock);
        pthread_exit(NULL);
    }

    strcpy(clients[clientCount].username, username);
    clients[clientCount].socket = sock;
    clientCount++;

    if (send(sock, "Login successful\n", 17, 0) < 0) {
        log_event("[ERROR] Failed to send login success message to socket %d: %s", sock, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to send login success message to socket %d: %s\n", sock, strerror(errno));
    }
    log_event("%s logged in", username);
    show_menu(sock);

    // Vòng lặp xử lý tin nhắn
    while (1) {
        if (sock < 0) {
            log_event("[ERROR] Invalid socket %d for %s", sock, username);
            fprintf(stderr, "[ERROR] Invalid socket %d for %s\n", sock, username);
            break;
        }
        len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len < 0) {
            log_event("[ERROR] Receive failed for %s: %s", username, strerror(errno));
            fprintf(stderr, "[ERROR] Receive failed for %s: %s\n", username, strerror(errno));
            break;
        } else if (len == 0) {
            log_event("%s disconnected: Connection closed", username);
            fprintf(stderr, "%s disconnected: Connection closed\n", username);
            break;
        }
        buffer[len] = '\0';
        log_event("Received from %s: %s", username, buffer);

        if (strncmp(buffer, "/exit", 5) == 0) {
            break;
        }
        else if (strncmp(buffer, "/menu", 5) == 0) {
            show_menu(sock);
            continue;
        }
        else if (strncmp(buffer, "/users", 6) == 0) {
            show_users(sock);
            continue;
        }
        else if (strncmp(buffer, "/groups", 7) == 0) {
            show_groups_for_user(sock, username);
            continue;
        }
        else if (buffer[0] == '/') {
            char target[32] = {0}, msg[BUFFER_SIZE] = {0};
            char *space = strchr(buffer + 1, ' ');
            if (space) {
                strncpy(target, buffer + 1, space - (buffer + 1));
                target[space - (buffer + 1)] = '\0';
                strcpy(msg, space + 1);
            } else {
                strcpy(target, buffer + 1);
            }
            log_event("Parsed command from %s: target=%s, msg='%s'", username, target, msg);

            if (strcmp(target, "menu") == 0 || strcmp(target, "users") == 0 ||
                strcmp(target, "groups") == 0 || strcmp(target, "exit") == 0) {
                if (send(sock, "[Server] Invalid command format.\n", 33, 0) < 0) {
                    log_event("[ERROR] Failed to send invalid command message to socket %d: %s", sock, strerror(errno));
                    fprintf(stderr, "[ERROR] Failed to send invalid command message to socket %d: %s\n", sock, strerror(errno));
                }
                continue;
            }

            int isGroup = is_user_in_group(target, username);

            if (strlen(msg) > 0) {
                if (isGroup) {
                    log_event("Sending group message to %s: %s", target, msg);
                    send_group_message(username, target, msg);
                } else if (find_client_by_name(target)) {
                    log_event("Sending private message to %s: %s", target, msg);
                    send_private(username, target, msg);
                } else {
                    log_event("Invalid target: %s", target);
                    if (send(sock, "[Server] Invalid target.\n", 26, 0) < 0) {
                        log_event("[ERROR] Failed to send invalid target message to socket %d: %s", sock, strerror(errno));
                        fprintf(stderr, "[ERROR] Failed to send invalid target message to socket %d: %s\n", sock, strerror(errno));
                    }
                }
            }
        }
        else if (buffer[0] == '|') {
            char target[32] = {0};
            strncpy(target, buffer + 1, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0'; // Đảm bảo chuỗi kết thúc
            log_event("Fetching conversation history for %s", target);
            int isGroup = is_user_in_group(target, username);
            send_conversation_history(sock, username, target, isGroup);
        }
        else {
            log_event("Broadcasting message from %s: %s", username, buffer);
            broadcast(username, buffer);
        }
    }

    remove_client(sock);
    pthread_exit(NULL);
}

// ========================= MAIN =========================
int main() {
    printf("=== IPC CHAT SERVER (SOCKET MODE) ===\n");

    // Mở file log trước khi gọi bất kỳ hàm nào sử dụng log_event
    logFile = fopen("server.log", "a");
    if (!logFile) {
        fprintf(stderr, "[ERROR] Failed to open server.log: %s\n", strerror(errno));
        exit(1);
    }
    // Tạo thư mục conversation đệ quy nếu cần
    if (mkdir("conversation", 0777) == -1) {
        if (errno != EEXIST) {
            log_event("[ERROR] Failed to create conversation directory: %s", strerror(errno));
            fprintf(stderr, "[ERROR] Failed to create conversation directory: %s\n", strerror(errno));
            if (errno == ENOENT) {
                log_event("Attempting to create parent directories...");
                char *dir = "conversation";
                char tmp[256];
                char *p = NULL;
                size_t len;

                snprintf(tmp, sizeof(tmp), "%s", dir);
                len = strlen(tmp);
                if (tmp[len - 1] == '/')
                    tmp[len - 1] = 0;
                for (p = tmp + 1; *p; p++)
                    if (*p == '/') {
                        *p = 0;
                        if (mkdir(tmp, 0777) == -1 && errno != EEXIST) {
                            log_event("[ERROR] Failed to create directory %s: %s", tmp, strerror(errno));
                            fprintf(stderr, "[ERROR] Failed to create directory %s: %s\n", tmp, strerror(errno));
                            exit(1);
                        }
                        *p = '/';
                    }
                if (mkdir(tmp, 0777) == -1) {
                    log_event("[ERROR] Failed to create final directory %s: %s", tmp, strerror(errno));
                    fprintf(stderr, "[ERROR] Failed to create final directory %s: %s\n", tmp, strerror(errno));
                    exit(1);
                }
            } else {
                exit(1);
            }
        }
    }
    log_event("Conversation directory created or exists");

    load_users();
    load_groups();
    logFile = fopen("server.log", "a");
    if (!logFile) {
        fprintf(stderr, "[ERROR] Failed to open server.log: %s\n", strerror(errno));
        exit(1);
    }
    log_event("Server started");

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        log_event("[ERROR] Socket creation failed: %s", strerror(errno));
        fprintf(stderr, "[ERROR] Socket creation failed: %s\n", strerror(errno));
        fclose(logFile);
        exit(1);
    }
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_event("[ERROR] Setsockopt failed: %s", strerror(errno));
        fprintf(stderr, "[ERROR] Setsockopt failed: %s\n", strerror(errno));
        fclose(logFile);
        close(server_sock);
        exit(1);
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_event("[ERROR] Bind failed: %s", strerror(errno));
        fprintf(stderr, "[ERROR] Bind failed: %s\n", strerror(errno));
        fclose(logFile);
        close(server_sock);
        exit(1);
    }
    if (listen(server_sock, 5) < 0) {
        log_event("[ERROR] Listen failed: %s", strerror(errno));
        fprintf(stderr, "[ERROR] Listen failed: %s\n", strerror(errno));
        fclose(logFile);
        close(server_sock);
        exit(1);
    }
    printf("Server started on port %d\n", PORT);
    log_event("Server listening on port %d", PORT);

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) {
            log_event("[ERROR] Accept failed: %s", strerror(errno));
            fprintf(stderr, "[ERROR] Accept failed: %s\n", strerror(errno));
            continue;
        }
        log_event("New client connected: socket %d", client_sock);
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, &client_sock) != 0) {
            log_event("[ERROR] Failed to create client thread: %s", strerror(errno));
            fprintf(stderr, "[ERROR] Failed to create client thread: %s\n", strerror(errno));
            close(client_sock);
            continue;
        }
        pthread_detach(tid);
    }

    fclose(logFile);
    close(server_sock);
    return 0;
}

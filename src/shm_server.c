#include "../include/shm_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define MAX_CLIENTS 100

ClientInfo clients[MAX_CLIENTS];
int clientCount = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

SharedMemory *shm = NULL;
int shm_fd = -1;
volatile int server_running = 1;

// ========================= HELPER FUNCTIONS =========================
ClientInfo *find_client_by_name(const char *username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < clientCount; i++) {
        if (strcmp(clients[i].username, username) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void send_to_client(int client_id, const char *message) {
    write_to_client(shm, client_id, message);
}

int check_login(const char *username, const char *password) {
    for (int i = 0; i < userCount; i++) {
        if (strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password, password) == 0)
            return 1;
    }
    return 0;
}

void remove_client(int client_id, const char *username) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < clientCount; i++) {
        if (clients[i].client_id == client_id) {
            log_event("%s disconnected", username);
            reset_client_slot(shm, client_id);
            for (int j = i; j < clientCount - 1; j++)
                clients[j] = clients[j + 1];
            clientCount--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// ========================= MESSAGE HANDLERS =========================
void broadcast(const char *sender, const char *msg) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "[%s -> ALL]: %s", sender, msg);
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < clientCount; i++) {
        write_to_client(shm, clients[i].client_id, buffer);
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_private(const char *sender, const char *target, const char *msg) {
    ClientInfo *receiver = find_client_by_name(target);
    char buffer[BUFFER_SIZE];
    
    if (receiver) {
        snprintf(buffer, sizeof(buffer), "[PM %s → %s]: %s", sender, target, msg);
        write_to_client(shm, receiver->client_id, buffer);
        save_conversation(sender, target, msg, 0);
    } else {
        snprintf(buffer, sizeof(buffer), "[Server] User %s not found.\n", target);
        ClientInfo *senderClient = find_client_by_name(sender);
        if (senderClient) {
            write_to_client(shm, senderClient->client_id, buffer);
        }
    }
}

void send_group_message(const char *sender, const char *groupId, const char *msg) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "[%s@%s]: %s", sender, groupId, msg);
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < clientCount; i++) {
        if (is_user_in_group(groupId, clients[i].username)) {
            write_to_client(shm, clients[i].client_id, buffer);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    save_conversation(sender, groupId, msg, 1);
}

void show_menu(int client_id) {
    const char *menu =
        "\n=== MENU ===\n"
        "/menu /users /groups\n"
        "|<user> |<group> - history\n"
        "/<user> <msg> - private\n"
        "/<group> <msg> - group\n"
        "/exit - logout\n";
    send_to_client(client_id, menu);
}

void show_users(int client_id) {
    char buffer[BUFFER_SIZE] = "=== Users ===\n";
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < clientCount; i++) {
        strcat(buffer, clients[i].username);
        strcat(buffer, "\n");
    }
    pthread_mutex_unlock(&clients_mutex);
    send_to_client(client_id, buffer);
}

void show_groups_for_user(int client_id, const char *username) {
    char buffer[BUFFER_SIZE] = "=== Groups ===\n";
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
    if (!found) strcat(buffer, "(None)\n");
    send_to_client(client_id, buffer);
}

// ========================= LOGIN HANDLER =========================
int handle_login(int client_id, char *username_out) {
    char buffer[BUFFER_SIZE];
    int timeout = 100; // 10s
    
    while (timeout-- > 0 && server_running) {
        int len = read_from_client(shm, client_id, buffer, BUFFER_SIZE);
        if (len > 0) {
            char username[32], password[32];
            if (sscanf(buffer, "%31[^:]:%31s", username, password) == 2) {
                if (!check_login(username, password)) {
                    send_to_client(client_id, "❌ Invalid credentials\n");
                    usleep(200000);
                    reset_client_slot(shm, client_id);
                    return 0;
                }
                
                if (find_client_by_name(username)) {
                    send_to_client(client_id, "❌ Username in use\n");
                    usleep(200000);
                    reset_client_slot(shm, client_id);
                    return 0;
                }
                
                strcpy(username_out, username);
                
                pthread_mutex_lock(&clients_mutex);
                strcpy(clients[clientCount].username, username);
                clients[clientCount].client_id = client_id;
                clients[clientCount].active = 1;
                clientCount++;
                pthread_mutex_unlock(&clients_mutex);
                
                mark_slot_authenticated(shm, client_id);
                send_to_client(client_id, "✅ Login OK\n");
                show_menu(client_id);
                log_event("%s logged in (slot %d)", username, client_id);
                return 1;
            }
        }
        usleep(100000); // 100ms
    }
    
    send_to_client(client_id, "⏱️ Timeout\n");
    usleep(200000);
    reset_client_slot(shm, client_id);
    return 0;
}

// ========================= MESSAGE HANDLER =========================
void handle_message(int client_id, const char *username, const char *buffer) {
    //  XỬ LÝ PING - chỉ cập nhật last_activity (đã được cập nhật ở main loop)
    if (strncmp(buffer, "/ping", 5) == 0) {
        // Silent heartbeat - không phản hồi gì cả
        return;
    }
    
    if (strncmp(buffer, "/exit", 5) == 0) {
        remove_client(client_id, username);
        return;
    }
    
    if (strncmp(buffer, "/menu", 5) == 0) {
        show_menu(client_id);
    }
    else if (strncmp(buffer, "/users", 6) == 0) {
        show_users(client_id);
    }
    else if (strncmp(buffer, "/groups", 7) == 0) {
        show_groups_for_user(client_id, username);
    }
    else if (buffer[0] == '/') {
        char target[32] = {0}, msg[BUFFER_SIZE] = {0};
        char *space = strchr(buffer + 1, ' ');
        if (space) {
            strncpy(target, buffer + 1, space - (buffer + 1));
            strcpy(msg, space + 1);
        } else {
            strcpy(target, buffer + 1);
        }

        if (strlen(msg) > 0) {
            int isGroup = is_user_in_group(target, username);
            if (isGroup) {
                send_group_message(username, target, msg);
            } else if (find_client_by_name(target)) {
                send_private(username, target, msg);
            } else {
                send_to_client(client_id, "[Server] Invalid target\n");
            }
        }
    }
    else if (buffer[0] == '|') {
        char target[32] = {0};
        strncpy(target, buffer + 1, sizeof(target) - 1);
        int isGroup = is_user_in_group(target, username);
        send_conversation_history_shm(shm, client_id, username, target, isGroup);
    }
    else {
        broadcast(username, buffer);
    }
}

void cleanup(int sig) {
    server_running = 0;
    log_event("Server shutdown");
    
    if (shm) {
        // Destroy semaphores trước khi cleanup
        for (int i = 0; i < MAX_CLIENTS; i++) {
            sem_destroy(&shm->clients[i].sem_client_write);
            sem_destroy(&shm->clients[i].sem_server_read);
            sem_destroy(&shm->clients[i].sem_server_write);
            sem_destroy(&shm->clients[i].sem_client_read);
            pthread_mutex_destroy(&shm->clients[i].mutex);
        }
        pthread_mutex_destroy(&shm->global_mutex);
        
        cleanup_shm(shm_fd, shm);
        shm_unlink(SHM_NAME);
    }
    
    if (logFile) fclose(logFile);
    exit(0);
}

// ========================= MAIN - PURE POLLING =========================
int main() {
    printf("=== CHAT SERVER (Shared Memory + Semaphore + Heartbeat) ===\n");

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    logFile = fopen("server_shm.log", "a");
    if (!logFile) {
        fprintf(stderr, "Cannot open log\n");
        exit(1);
    }

    if (mkdir("conversation", 0777) == -1 && errno != EEXIST) {
        log_event("mkdir conversation failed");
    }

    load_users();
    load_groups();
    log_event("Server started");

    shm = init_shm(&shm_fd);
    if (!shm) {
        log_event("init_shm failed");
        exit(1);
    }

    printf("✅ Server ready (Pure shared memory + semaphore)\n");
    printf("💓 Heartbeat timeout: 30 minutes\n");
    printf("📊 %d slots available\n\n", MAX_CLIENTS);

    // ✅ Slot tracking
    typedef struct {
        int authenticated;
        char username[32];
        time_t last_activity;
    } SlotInfo;
    
    SlotInfo slots[MAX_CLIENTS] = {0};
    
    //  PURE POLLING LOOP - CHỈ 1 THREAD
    while (server_running) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            pthread_mutex_lock(&shm->clients[i].mutex);
            SlotState state = shm->clients[i].state;
            pthread_mutex_unlock(&shm->clients[i].mutex);
            
            // New connection
            if (state == SLOT_CONNECTING && !slots[i].authenticated) {
                log_event("New client on slot %d", i);
                char username[32] = {0};
                if (handle_login(i, username)) {
                    slots[i].authenticated = 1;
                    strcpy(slots[i].username, username);
                    slots[i].last_activity = time(NULL);
                } else {
                    slots[i].authenticated = 0;
                }
            }
            // Authenticated client
            else if (state == SLOT_AUTHENTICATED && slots[i].authenticated) {
                char buffer[BUFFER_SIZE];
                int len = read_from_client(shm, i, buffer, BUFFER_SIZE);
                
                if (len > 0) {
                    // CẬP NHẬT last_activity cho MỌI tin nhắn (kể cả /ping)
                    slots[i].last_activity = time(NULL);
                    
                    if (strncmp(buffer, "/exit", 5) == 0) {
                        remove_client(i, slots[i].username);
                        slots[i].authenticated = 0;
                        memset(slots[i].username, 0, 32);
                    } else {
                        handle_message(i, slots[i].username, buffer);
                    }
                }
            }
            // Slot freed
            else if (state == SLOT_FREE && slots[i].authenticated) {
                slots[i].authenticated = 0;
                memset(slots[i].username, 0, 32);
            }
        }
        
        //  Timeout check - 30 PHÚT (1800 giây)
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (slots[i].authenticated && 
                (now - slots[i].last_activity) > 1800) {
                log_event("Slot %d (%s) timeout after 30min", i, slots[i].username);
                send_to_client(i, "[Server] ⏱️ Timeout (30 minutes inactive)\n");
                usleep(100000);
                remove_client(i, slots[i].username);
                slots[i].authenticated = 0;
            }
        }
        
        usleep(50000); // 50ms polling interval
    }

    cleanup(0);
    return 0;
}

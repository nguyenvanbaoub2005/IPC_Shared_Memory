#include "../include/shm_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

SharedMemory *shm = NULL;
int shm_fd = -1;
int my_client_id = -1;
volatile int running = 1;


void *recv_thread(void *arg) {
    char buffer[BUFFER_SIZE];
    char *full_msg = NULL;
    size_t total_len = 0;

    while (running) {
        int len = read_from_server(shm, my_client_id, buffer, BUFFER_SIZE);
        
        if (len < 0) {
            printf("\n[Connection lost]\n");
            running = 0;
            break;
        }
        
        if (len > 0) {
            full_msg = realloc(full_msg, total_len + len + 1);
            memcpy(full_msg + total_len, buffer, len);
            total_len += len;
            full_msg[total_len] = '\0';

            if (strstr(full_msg, "=== End of History ===\n") || 
                strstr(full_msg, "\n") || total_len < BUFFER_SIZE - 1) {
                printf("%s", full_msg);
                if (full_msg[strlen(full_msg) - 1] != '\n') {
                    printf("\n");
                }
                free(full_msg);
                full_msg = NULL;
                total_len = 0;
            }
            fflush(stdout);
        }
        usleep(50000); // 50ms
    }

    if (full_msg) free(full_msg);
    pthread_exit(NULL);
}

int main() {
    printf("=== CHAT CLIENT (Shared Memory + Semaphore) ===\n");

    shm = connect_shm(&shm_fd);
    if (!shm) {
        fprintf(stderr, " Cannot connect. Is server running?\n");
        return 1;
    }

    my_client_id = find_free_client_slot(shm);
    if (my_client_id < 0) {
        fprintf(stderr, " Server full\n");
        cleanup_shm(shm_fd, shm);
        return 1;
    }

    printf(" Connected (slot %d)\n", my_client_id);

    // Login
    char username[32], password[32], creds[64];
    printf("Username: "); 
    if (scanf("%31s", username) != 1) {
        reset_client_slot(shm, my_client_id);
        cleanup_shm(shm_fd, shm);
        return 1;
    }
    
    printf("Password: "); 
    if (scanf("%31s", password) != 1) {
        reset_client_slot(shm, my_client_id);
        cleanup_shm(shm_fd, shm);
        return 1;
    }
    getchar();

    snprintf(creds, sizeof(creds), "%s:%s", username, password);
    
    if (write_to_server(shm, my_client_id, creds) < 0) {
        fprintf(stderr, " Send failed\n");
        reset_client_slot(shm, my_client_id);
        cleanup_shm(shm_fd, shm);
        return 1;
    }
    
    printf(" Waiting...\n");

    // Wait for login response
    char response[128];
    int timeout = 100; // 10s
    int login_ok = 0;
    
    while (timeout-- > 0) {
        int len = read_from_server(shm, my_client_id, response, sizeof(response));
        
        if (len < 0) {
            printf(" Connection error\n");
            cleanup_shm(shm_fd, shm);
            return 0;
        }
        
        if (len > 0) {
            response[len] = '\0';
            
            if (strstr(response, "❌")) {
                printf("%s", response);
                cleanup_shm(shm_fd, shm);
                return 0;
            }
            
            if (strstr(response, "") || strstr(response, "OK")) {
                printf("%s", response);
                login_ok = 1;
                break;
            }
        }
        usleep(100000);
    }

    if (!login_ok) {
        printf("⏱️ Timeout\n");
        reset_client_slot(shm, my_client_id);
        cleanup_shm(shm_fd, shm);
        return 1;
    }


    // Start receive thread
    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, NULL) != 0) {
        printf(" Thread creation failed\n");
        reset_client_slot(shm, my_client_id);
        cleanup_shm(shm_fd, shm);
        return 1;
    }
    pthread_detach(tid);

    // Input loop
    char msg[BUFFER_SIZE];
    while (running) {
        printf("> ");
        fflush(stdout);

        if (!fgets(msg, sizeof(msg), stdin)) break;
        msg[strcspn(msg, "\n")] = 0;

        // Trim
        char *start = msg;
        while (*start == ' ') start++;
        char *end = start + strlen(start) - 1;
        while (end > start && *end == ' ') end--;
        *(end + 1) = '\0';

        if (strcmp(start, "/exit") == 0) {
            if (write_to_server(shm, my_client_id, "/exit") < 0) {
                printf("[Warning] Exit send failed\n");
            }
            running = 0;
            break;
        }

        if (strlen(start) == 0) continue;

        if (write_to_server(shm, my_client_id, start) < 0) {
            printf("[Error] Send failed\n");
            running = 0;
            break;
        }
    }

    usleep(500000);
    reset_client_slot(shm, my_client_id);
    cleanup_shm(shm_fd, shm);
    printf("\n Disconnected\n");
    return 0;
}

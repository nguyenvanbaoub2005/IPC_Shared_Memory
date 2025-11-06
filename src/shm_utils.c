#include "../include/shm_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

User users[100];
Group groups[50];
int userCount = 0;
int groupCount = 0;
FILE *logFile = NULL;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// SHARED MEMORY FUNCTIONS 
SharedMemory *init_shm(int *shm_fd) {
    shm_unlink(SHM_NAME);

    *shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (*shm_fd == -1) {
        fprintf(stderr, "[ERROR] shm_open failed: %s\n", strerror(errno));
        return NULL;
    }

    if (ftruncate(*shm_fd, sizeof(SharedMemory)) == -1) {
        fprintf(stderr, "[ERROR] ftruncate failed: %s\n", strerror(errno));
        close(*shm_fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }

    SharedMemory *shm = mmap(NULL, sizeof(SharedMemory), 
                             PROT_READ | PROT_WRITE, MAP_SHARED, *shm_fd, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "[ERROR] mmap failed: %s\n", strerror(errno));
        close(*shm_fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }

    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->global_mutex, &mutex_attr);

    // genarate SEMAPHORES for each slot
    for (int i = 0; i < MAX_CLIENTS; i++) { 
        memset(shm->clients[i].client_to_server_buf, 0, BUFFER_SIZE);
        memset(shm->clients[i].server_to_client_buf, 0, BUFFER_SIZE);
        
        shm->clients[i].state = SLOT_FREE;
        shm->clients[i].in_use = 0;
        
        pthread_mutex_init(&shm->clients[i].mutex, &mutex_attr);
        
        //  Semaphores with PTHREAD_PROCESS_SHARED
        sem_init(&shm->clients[i].sem_client_write, 1, 1);  // Client có thể write ngay
        sem_init(&shm->clients[i].sem_server_read, 1, 0);   // Server chưa có gì để read
        sem_init(&shm->clients[i].sem_server_write, 1, 1);  // Server có thể write ngay
        sem_init(&shm->clients[i].sem_client_read, 1, 0);   // Client chưa có gì để read
    }

    pthread_mutexattr_destroy(&mutex_attr);
    return shm;
}

SharedMemory *connect_shm(int *shm_fd) {
    *shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (*shm_fd == -1) {
        fprintf(stderr, "[ERROR] shm_open failed: %s\n", strerror(errno));
        return NULL;
    }

    SharedMemory *shm = mmap(NULL, sizeof(SharedMemory), 
                             PROT_READ | PROT_WRITE, MAP_SHARED, *shm_fd, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "[ERROR] mmap failed: %s\n", strerror(errno));
        close(*shm_fd);
        return NULL;
    }

    return shm;
}

void cleanup_shm(int shm_fd, SharedMemory *shm) {
    if (shm) {
        munmap(shm, sizeof(SharedMemory));
    }
    if (shm_fd >= 0) {
        close(shm_fd);
    }
}

int find_free_client_slot(SharedMemory *shm) {
    pthread_mutex_lock(&shm->global_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_lock(&shm->clients[i].mutex);
        if (shm->clients[i].state == SLOT_FREE) {
            shm->clients[i].state = SLOT_CONNECTING;
            shm->clients[i].in_use = 1;
            
            //  Reset semaphores
            sem_init(&shm->clients[i].sem_client_write, 1, 1);
            sem_init(&shm->clients[i].sem_server_read, 1, 0);
            sem_init(&shm->clients[i].sem_server_write, 1, 1);
            sem_init(&shm->clients[i].sem_client_read, 1, 0);
            
            pthread_mutex_unlock(&shm->clients[i].mutex);
            pthread_mutex_unlock(&shm->global_mutex);
            return i;
        }
        pthread_mutex_unlock(&shm->clients[i].mutex);
    }
    pthread_mutex_unlock(&shm->global_mutex);
    return -1;
}

void reset_client_slot(SharedMemory *shm, int client_id) {
    if (client_id < 0 || client_id >= MAX_CLIENTS) return;
    
    pthread_mutex_lock(&shm->clients[client_id].mutex);
    shm->clients[client_id].state = SLOT_FREE;
    shm->clients[client_id].in_use = 0;
    memset(shm->clients[client_id].client_to_server_buf, 0, BUFFER_SIZE);
    memset(shm->clients[client_id].server_to_client_buf, 0, BUFFER_SIZE);
    
    //  Reset semaphores
    sem_init(&shm->clients[client_id].sem_client_write, 1, 1);
    sem_init(&shm->clients[client_id].sem_server_read, 1, 0);
    sem_init(&shm->clients[client_id].sem_server_write, 1, 1);
    sem_init(&shm->clients[client_id].sem_client_read, 1, 0);
    
    pthread_mutex_unlock(&shm->clients[client_id].mutex);
}

int mark_slot_authenticated(SharedMemory *shm, int client_id) {
    if (client_id < 0 || client_id >= MAX_CLIENTS) return -1;
    
    pthread_mutex_lock(&shm->clients[client_id].mutex);
    if (shm->clients[client_id].state == SLOT_CONNECTING) {
        shm->clients[client_id].state = SLOT_AUTHENTICATED;
        pthread_mutex_unlock(&shm->clients[client_id].mutex);
        return 0;
    }
    pthread_mutex_unlock(&shm->clients[client_id].mutex);
    return -1;
}

// CLIENT WRITE → SERVER READ
int write_to_server(SharedMemory *shm, int client_id, const char *msg) {
    if (client_id < 0 || client_id >= MAX_CLIENTS) return -1;
    
    ClientSlot *slot = &shm->clients[client_id];
    
    // Wait cho phép write
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5; // 5s timeout
    
    if (sem_timedwait(&slot->sem_client_write, &ts) != 0) {
        return -1; // Timeout
    }
    
    strncpy(slot->client_to_server_buf, msg, BUFFER_SIZE - 1);
    slot->client_to_server_buf[BUFFER_SIZE - 1] = '\0';
    
    sem_post(&slot->sem_server_read); // Signal server có data
    return strlen(msg);
}

int read_from_client(SharedMemory *shm, int client_id, char *buffer, int size) {
    if (client_id < 0 || client_id >= MAX_CLIENTS) return -1;
    
    ClientSlot *slot = &shm->clients[client_id];
    
    // Try read (non-blocking với trywait)
    if (sem_trywait(&slot->sem_server_read) != 0) {
        return 0; // Không có data
    }
    
    strncpy(buffer, slot->client_to_server_buf, size - 1);
    buffer[size - 1] = '\0';
    int len = strlen(buffer);
    
    memset(slot->client_to_server_buf, 0, BUFFER_SIZE);
    sem_post(&slot->sem_client_write); // Cho phép client write lại
    
    return len;
}

// SERVER WRITE → CLIENT READ
int write_to_client(SharedMemory *shm, int client_id, const char *msg) {
    if (client_id < 0 || client_id >= MAX_CLIENTS) return -1;
    
    ClientSlot *slot = &shm->clients[client_id];
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    
    if (sem_timedwait(&slot->sem_server_write, &ts) != 0) {
        return -1;
    }
    
    strncpy(slot->server_to_client_buf, msg, BUFFER_SIZE - 1);
    slot->server_to_client_buf[BUFFER_SIZE - 1] = '\0';
    
    sem_post(&slot->sem_client_read);
    return strlen(msg);
}

int read_from_server(SharedMemory *shm, int client_id, char *buffer, int size) {
    if (client_id < 0 || client_id >= MAX_CLIENTS) return -1;
    
    ClientSlot *slot = &shm->clients[client_id];
    
    if (sem_trywait(&slot->sem_client_read) != 0) {
        return 0;
    }
    
    strncpy(buffer, slot->server_to_client_buf, size - 1);
    buffer[size - 1] = '\0';
    int len = strlen(buffer);
    
    memset(slot->server_to_client_buf, 0, BUFFER_SIZE);
    sem_post(&slot->sem_server_write);
    
    return len;
}

// ========================= USER/GROUP MANAGEMENT =========================
void log_event(const char *fmt, ...) {
    if (!logFile) return;
    
    va_list args;
    va_start(args, fmt);

    time_t now = time(NULL);
    char *t = ctime(&now);
    t[strcspn(t, "\n")] = 0;
    fprintf(logFile, "[%s] ", t);
    vfprintf(logFile, fmt, args);
    fprintf(logFile, "\n");
    fflush(logFile);

    va_end(args);
}

void load_users() {
    FILE *f = fopen("./data/user.txt", "r");
    if (!f) {
        fprintf(stderr, "[ERROR] Cannot open data/user.txt: %s\n", strerror(errno));
        exit(1);
    }
    while (fscanf(f, "%31[^:]:%31s\n", users[userCount].username, users[userCount].password) == 2) {
        userCount++;
    }
    fclose(f);
    log_event("Loaded %d users", userCount);
}

void load_groups() {
    FILE *f = fopen("./data/group.txt", "r");
    if (!f) {
        fprintf(stderr, "[ERROR] Cannot open data/group.txt: %s\n", strerror(errno));
        exit(1);
    }
    while (fscanf(f, "%31[^:]:%63[^:]:%255[^\n]\n",
                  groups[groupCount].groupId,
                  groups[groupCount].groupName,
                  groups[groupCount].members) == 3) {
        groupCount++;
    }
    fclose(f);
    log_event("Loaded %d groups", groupCount);
}

int is_user_in_group(const char *groupId, const char *username) {
    for (int i = 0; i < groupCount; i++) {
        if (strcmp(groups[i].groupId, groupId) == 0) {
            char tmp[256];
            strcpy(tmp, groups[i].members);
            char *tok = strtok(tmp, ",");
            while (tok) {
                if (strcmp(tok, username) == 0)
                    return 1;
                tok = strtok(NULL, ",");
            }
        }
    }
    return 0;
}

void save_conversation(const char *sender, const char *target, const char *msg, int isGroup) {
    pthread_mutex_lock(&file_mutex);
    
    char filename[256];
    if (isGroup)
        snprintf(filename, sizeof(filename), "conversation/conversation_%s.txt", target);
    else {
        const char *user1 = strcmp(sender, target) < 0 ? sender : target;
        const char *user2 = strcmp(sender, target) < 0 ? target : sender;
        snprintf(filename, sizeof(filename), "conversation/conversation_%s_%s.txt", user1, user2);
    }

    FILE *f = fopen(filename, "a");
    if (!f) {
        log_event("[ERROR] Failed to open %s: %s", filename, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    time_t now = time(NULL);
    char *t = ctime(&now);
    t[strcspn(t, "\n")] = 0;
    fprintf(f, "[%s] %s: %s\n", t, sender, msg);
    fclose(f);
    
    pthread_mutex_unlock(&file_mutex);
}

void send_conversation_history_shm(SharedMemory *shm, int client_id, const char *sender, const char *target, int isGroup) {
    pthread_mutex_lock(&file_mutex);
    
    char filename[256];
    if (isGroup)
        snprintf(filename, sizeof(filename), "conversation/conversation_%s.txt", target);
    else {
        const char *user1 = strcmp(sender, target) < 0 ? sender : target;
        const char *user2 = strcmp(sender, target) < 0 ? target : sender;
        snprintf(filename, sizeof(filename), "conversation/conversation_%s_%s.txt", user1, user2);
    }

    FILE *f = fopen(filename, "r");
    if (!f) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[Server] No history with %s.\n", target);
        write_to_client(shm, client_id, msg);
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    char header[128];
    snprintf(header, sizeof(header), "\n=== History with %s ===\n", target);
    write_to_client(shm, client_id, header);
    usleep(10000);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;
        
        write_to_client(shm, client_id, line);
        write_to_client(shm, client_id, "\n");
        usleep(10000);
    }

    write_to_client(shm, client_id, "=== End of History ===\n");
    fclose(f);
    pthread_mutex_unlock(&file_mutex);
}

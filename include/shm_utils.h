#ifndef SHM_UTILS_H
#define SHM_UTILS_H
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>     
#include <sys/stat.h>     
#include <fcntl.h>        
#include <string.h>       
#include <unistd.h>        

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100
#define SHM_NAME "/chat_shm"

typedef struct {
    char username[32];
    char password[32];
} User;

typedef struct {
    char groupId[32];
    char groupName[64];
    char members[256];
} Group;

typedef struct {
    int client_id;
    char username[32];
    int active;
} ClientInfo;

typedef enum {
    SLOT_FREE = 0,
    SLOT_CONNECTING = 1,
    SLOT_AUTHENTICATED = 2
} SlotState;

// ClientSlot với SEMAPHORE thay vì condition variables
typedef struct {
    // Communication buffers
    char client_to_server_buf[BUFFER_SIZE];
    char server_to_client_buf[BUFFER_SIZE];
    
    // SEMAPHORES cho synchronization
    sem_t sem_client_write;   // Client có thể write
    sem_t sem_server_read;    // Server có thể read
    sem_t sem_server_write;   // Server có thể write  
    sem_t sem_client_read;    // Client có thể read
    
    // State management
    SlotState state;
    int in_use;
    
    pthread_mutex_t mutex;  // Chỉ dùng để protect state
} ClientSlot;

typedef struct {
    ClientSlot clients[MAX_CLIENTS];
    pthread_mutex_t global_mutex;
} SharedMemory;

extern User users[100];
extern Group groups[50];
extern int userCount;
extern int groupCount;
extern FILE *logFile;
extern pthread_mutex_t file_mutex;

// Shared memory functions
SharedMemory *init_shm(int *shm_fd);
SharedMemory *connect_shm(int *shm_fd);
void cleanup_shm(int shm_fd, SharedMemory *shm);
int find_free_client_slot(SharedMemory *shm);
void reset_client_slot(SharedMemory *shm, int client_id);
int mark_slot_authenticated(SharedMemory *shm, int client_id);

// Semaphore-based communication
int write_to_server(SharedMemory *shm, int client_id, const char *msg);
int read_from_server(SharedMemory *shm, int client_id, char *buffer, int size);
int write_to_client(SharedMemory *shm, int client_id, const char *msg);
int read_from_client(SharedMemory *shm, int client_id, char *buffer, int size);

// User/Group management
void load_users();
void load_groups();
void log_event(const char *fmt, ...);
int is_user_in_group(const char *groupId, const char *username);
void save_conversation(const char *sender, const char *target, const char *msg, int isGroup);
void send_conversation_history_shm(SharedMemory *shm, int client_id, const char *sender, const char *target, int isGroup);

#endif

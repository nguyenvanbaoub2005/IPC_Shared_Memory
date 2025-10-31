#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

#include <stdio.h>
#include <errno.h>
#include <pthread.h>

typedef struct {
    char username[32];
    char password[32];
} User;

typedef struct {
    char groupId[32];
    char groupName[64];
    char members[256];
} Group;

extern User users[100];
extern Group groups[50];
extern int userCount;
extern int groupCount;
extern FILE *logFile;
extern pthread_mutex_t file_mutex;

void load_users();
void load_groups();
void log_event(const char *fmt, ...);
int is_user_in_group(const char *groupId, const char *username);
void save_conversation(const char *sender, const char *target, const char *msg, int isGroup);
void send_conversation_history(int sock, const char *sender, const char *target, int isGroup);

#endif
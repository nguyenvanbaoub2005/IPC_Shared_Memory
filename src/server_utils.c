#include "../include/server_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

User users[100];
Group groups[50];
int userCount = 0;
int groupCount = 0;
FILE *logFile = NULL;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_event(const char *fmt, ...) {
    if (!logFile) {
        fprintf(stderr, "[ERROR] logFile is NULL in log_event: %s\n", strerror(errno));
        return;
    }
    va_list args;
    va_start(args, fmt);

    time_t now = time(NULL);
    char *t = ctime(&now);
    t[strcspn(t, "\n")] = 0;
    fprintf(logFile, "[%s] ", t);
    vfprintf(logFile, fmt, args);
    fprintf(logFile, "\n");
    if (fflush(logFile) != 0) {
        fprintf(stderr, "[ERROR] Failed to flush logFile: %s\n", strerror(errno));
    }

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
    log_event("Loaded %d users from user.txt", userCount);
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
    log_event("Loaded %d groups from group.txt", groupCount);
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
    if (mkdir("conversation", 0777) == -1 && errno != EEXIST) {
        log_event("[ERROR] Failed to create conversation directory: %s", strerror(errno));
        fprintf(stderr, "[ERROR] Failed to create conversation directory: %s\n", strerror(errno));
    }

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
        log_event("[ERROR] Failed to open conversation file %s for writing: %s", filename, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to open conversation file %s for writing: %s\n", filename, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    time_t now = time(NULL);
    char *t = ctime(&now);
    t[strcspn(t, "\n")] = 0;
    fprintf(f, "[%s] %s: %s\n", t, sender, msg);
    if (fflush(f) != 0) {
        log_event("[ERROR] Failed to flush conversation file %s: %s", filename, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to flush conversation file %s: %s\n", filename, strerror(errno));
    }
    if (fclose(f) != 0) {
        log_event("[ERROR] Failed to close conversation file %s: %s", filename, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to close conversation file %s: %s\n", filename, strerror(errno));
    }
    log_event("Saved conversation to %s: %s: %s", filename, sender, msg);
    pthread_mutex_unlock(&file_mutex);
}

void send_conversation_history(int sock, const char *sender, const char *target, int isGroup) {
    pthread_mutex_lock(&file_mutex);
    char filename[256];
    if (isGroup)
        snprintf(filename, sizeof(filename), "conversation/conversation_%s.txt", target);
    else {
        const char *user1 = strcmp(sender, target) < 0 ? sender : target;
        const char *user2 = strcmp(sender, target) < 0 ? target : sender;
        snprintf(filename, sizeof(filename), "conversation/conversation_%s_%s.txt", user1, user2);
    }
    log_event("Attempting to read conversation file: %s", filename);

    FILE *f = fopen(filename, "r");
    if (!f) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[Server] No conversation history with %s.\n", target);
        if (send(sock, msg, strlen(msg), 0) < 0) {
            log_event("[ERROR] Failed to send no history message to socket %d: %s", sock, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send no history message to socket %d: %s\n", sock, strerror(errno));
        }
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    char header[128];
    snprintf(header, sizeof(header), "\n=== History with %s ===\n", target);
    if (send(sock, header, strlen(header), 0) < 0) {
        log_event("[ERROR] Failed to send history header to socket %d: %s", sock, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to send history header to socket %d: %s\n", sock, strerror(errno));
        fclose(f);
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    char line[512];
    int lines_sent = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;
        log_event("Sending history line: %s", line);
        if (send(sock, line, strlen(line), 0) < 0 || send(sock, "\n", 1, 0) < 0) {
            log_event("[ERROR] Failed to send history line to socket %d: %s", sock, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send history line to socket %d: %s\n", sock, strerror(errno));
            break;
        }
        lines_sent++;
    }

    if (lines_sent == 0) {
        if (send(sock, "No messages found.\n", 19, 0) < 0) {
            log_event("[ERROR] Failed to send no messages message to socket %d: %s", sock, strerror(errno));
            fprintf(stderr, "[ERROR] Failed to send no messages message to socket %d: %s\n", sock, strerror(errno));
        }
    }

    if (send(sock, "=== End of History ===\n", 24, 0) < 0) {
        log_event("[ERROR] Failed to send history footer to socket %d: %s", sock, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to send history footer to socket %d: %s\n", sock, strerror(errno));
    }

    if (fclose(f) != 0) {
        log_event("[ERROR] Failed to close file %s: %s", filename, strerror(errno));
        fprintf(stderr, "[ERROR] Failed to close file %s: %s\n", filename, strerror(errno));
    }
    log_event("Sent conversation history for %s to socket %d (lines sent: %d)", target, sock, lines_sent);
    pthread_mutex_unlock(&file_mutex);
}
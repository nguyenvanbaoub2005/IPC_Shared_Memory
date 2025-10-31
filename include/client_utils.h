#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#define BUFFER_SIZE 1024

void print_menu();
void handle_server_message(int sock);
void handle_user_input(int sock, const char *username);

#endif
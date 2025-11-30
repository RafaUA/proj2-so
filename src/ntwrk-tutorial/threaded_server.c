#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define PORT 8080

void* handle_client(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);
    
    // Read request, send response
    char buffer[4096];
    recv(client_fd, buffer, sizeof(buffer), 0);

    const char* response = "HTTP/1.1 200 OK\r\n\r\n<h1>Hello</h1>";
    send(client_fd, response, strlen(response), 0);

    close(client_fd);
    return NULL;
}

int main() {
    // Socket setup
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);
    
    printf("Server listening on port %d...\n", PORT);

    while (1) {
        int* client_fd = malloc(sizeof(int));
        *client_fd = accept(server_fd, NULL, NULL);
        
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, client_fd);
        pthread_detach(thread); // Don't wait for thread to finish
    }
    
    close(server_fd);
    return 0;
}
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h> 
#include <netinet/in.h>

const char* response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "\r\n"
    "<html><body><h1>Hello, World!</h1></body></html>";

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 10);
    printf("HTTP server running on http://localhost:8080\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);

        char buffer[4096];
        recv(client_fd, buffer, sizeof(buffer), 0);

        send(client_fd, response, strlen(response), 0);
        close(client_fd);
    }
    
    return 0;
}
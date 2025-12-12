#include "http.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <time.h>
#include <semaphore.h>

int parse_http_request(const char* buffer, http_request_t* req) {
    char* line_end = strstr(buffer, "\r\n");
    if (!line_end) return -1;

    char first_line[1024];
    size_t len = line_end - buffer;
    strncpy(first_line, buffer, len);
    first_line[len] = '\0';

    if (sscanf(first_line, "%s %s %s", req->method, req->path, req->version) != 3) {
        return -1;
    }

    return 0;
}

void send_http_response(int client_fd, int status_code, const char* status_msg,
    const char* content_type, const char* body, size_t 
    body_len, int keep_alive) {
    char header[2048];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status_code, status_msg, content_type, body_len,
        keep_alive ? "keep-alive" : "close");

    send(client_fd, header, header_len, 0);

    if (body && body_len > 0) {
        send(client_fd, body, body_len, 0);
    }
}

void log_request(sem_t* log_sem, const char* client_ip, const char* method,
    const char* path, int status, size_t bytes) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%d/%b/%Y:%H:%M:%S %z", tm_info);

    sem_wait(log_sem);
    FILE* log = fopen("access.log", "a");
    if (log) {
        fprintf(log, "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu\n",
        client_ip, timestamp, method, path, status, bytes);
        fclose(log);
    }
    sem_post(log_sem);
}

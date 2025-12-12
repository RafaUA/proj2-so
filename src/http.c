#include "http.h"

#include <stdio.h>
#include <stdlib.h>
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
        "Accept-Ranges: bytes\r\n"
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

int parse_range_header(const char* range_value, range_request_t* range, size_t file_size) {
    if (!range_value || !range) return -1;

    range->has_range = 0;
    range->start = 0;
    range->end = 0;
    range->is_suffix_range = 0;

    if (strncmp(range_value, "bytes=", 6) != 0) {
        return -1;
    }

    const char* range_spec = range_value + 6;

    if (range_spec[0] == '-') {
        range->is_suffix_range = 1;
        long suffix_len = atol(range_spec + 1);
        if (suffix_len <= 0 || (size_t)suffix_len > file_size) {
            return -1;
        }
        range->start = file_size - suffix_len;
        range->end = file_size - 1;
        range->has_range = 1;
        return 0;
    }

    char* dash = strchr(range_spec, '-');
    if (!dash) {
        return -1;
    }

    range->start = atol(range_spec);

    if (*(dash + 1) == '\0' || *(dash + 1) == '\r' || *(dash + 1) == '\n') {
        range->end = file_size - 1;
    } else {
        range->end = atol(dash + 1);
    }

    if (range->start < 0 || range->end < 0 ||
        range->start > range->end ||
        (size_t)range->start >= file_size) {
        return -1;
    }

    if ((size_t)range->end >= file_size) {
        range->end = file_size - 1;
    }

    range->has_range = 1;
    return 0;
}

void send_http_response_range(int client_fd,
                               const char* content_type,
                               const char* body,
                               size_t total_size,
                               long range_start,
                               long range_end,
                               int keep_alive) {
    size_t content_length = range_end - range_start + 1;

    char header[2048];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Range: bytes %ld-%ld/%zu\r\n"
        "Accept-Ranges: bytes\r\n"
        "Server: ConcurrentHTTP/1.0\r\n"
        "Connection: %s\r\n"
        "\r\n",
        content_type, content_length, range_start, range_end, total_size,
        keep_alive ? "keep-alive" : "close");

    send(client_fd, header, header_len, 0);

    if (body && content_length > 0) {
        send(client_fd, body + range_start, content_length, 0);
    }
}

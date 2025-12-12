#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>

#define MAX_METHOD_LEN 16
#define MAX_PATH_LEN   512
#define MAX_VERSION_LEN 16

typedef struct {
    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    char version[MAX_VERSION_LEN];
} http_request_t;

typedef struct {
    int has_range;
    long start;
    long end;
    int is_suffix_range;
} range_request_t;

int parse_http_request(const char* buffer, http_request_t* req);

int parse_range_header(const char* range_value, range_request_t* range, size_t file_size);

void send_http_response_range(int client_fd,
                               const char* content_type,
                               const char* body,
                               size_t total_size,
                               long range_start,
                               long range_end,
                               int keep_alive);

void send_http_response(int client_fd,
                        int status_code,
                        const char* status_msg,
                        const char* content_type,
                        const char* body,
                        size_t body_len,
                        int keep_alive);

void log_request(sem_t* log_sem, const char* client_ip, const char* method, const char* path, int status, size_t bytes);

#endif 

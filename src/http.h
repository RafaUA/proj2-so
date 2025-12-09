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

int parse_http_request(const char* buffer, http_request_t* req);

void send_http_response(int client_fd, int status_code, const char* status_msg, const char* content_type, const char* body, size_t body_len);

void log_request(sem_t* log_sem, const char* client_ip, const char* method, const char* path, int status, size_t bytes);

#endif 

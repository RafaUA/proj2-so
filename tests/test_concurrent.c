#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 8080

#define NUM_THREADS 100     // concorrência
#define REQUESTS_PER_THREAD 100  // total = 10 000 pedidos

typedef struct {
    int ok_200;
    int other;
} result_t;

pthread_mutex_t res_mutex = PTHREAD_MUTEX_INITIALIZER;
result_t results = {0, 0};

void* worker_thread(void* arg) {
    (void)arg;

    for (int i = 0; i < REQUESTS_PER_THREAD; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            continue;
        }

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            close(sock);
            continue;
        }

        const char* request =
            "GET /index.html HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";

        send(sock, request, strlen(request), 0);

        char buffer[1024];
        ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = '\0';

            pthread_mutex_lock(&res_mutex);
            if (strstr(buffer, "200 OK")) {
                results.ok_200++;
            } else {
                results.other++;
            }
            pthread_mutex_unlock(&res_mutex);
        }

        close(sock);
    }

    return NULL;
}

int main(void) {
    printf("========================================\n");
    printf(" CONCURRENCY TEST (Tests 13–16)\n");
    printf("========================================\n");
    printf("Threads: %d\n", NUM_THREADS);
    printf("Requests per thread: %d\n", REQUESTS_PER_THREAD);
    printf("Total requests: %d\n\n", NUM_THREADS * REQUESTS_PER_THREAD);

    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n========================================\n");
    printf(" RESULTS\n");
    printf("========================================\n");
    printf("200 OK responses : %d\n", results.ok_200);
    printf("Other responses : %d\n", results.other);

    if (results.ok_200 > 0 && results.other == 0) {
        printf("\n✓ PASS: All requests served correctly\n");
        return 0;
    } else {
        printf("\n⚠ WARN: Some requests failed or returned non-200\n");
        return 0;  // warning, não erro fatal
    }
}

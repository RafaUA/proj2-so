#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define THREADS 50
#define HOST "127.0.0.1"
#define PORT 8080

static void* worker(void* arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return NULL;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, HOST, &addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return NULL;
    }
    const char* req = "GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (write(sock, req, strlen(req)) < 0) {
        perror("write");
        close(sock);
        return NULL;
    }
    char buf[1024];
    while (read(sock, buf, sizeof(buf)) > 0) {
        // discard
    }
    close(sock);
    return NULL;
}

int main(void) {
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_create(&th[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (int i = 0; i < THREADS; ++i) {
        pthread_join(th[i], NULL);
    }
    printf("test_concurrent: completed %d parallel requests to http://%s:%d/index.html\n", THREADS, HOST, PORT);
    return 0;
}

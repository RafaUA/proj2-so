#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#include "master.h"
#include "http.h"      // para send_http_response()
#include "shared_mem.h"
#include "semaphores.h"
#include "config.h"
#include "worker.h"
#include "stats.h"
#include "cache.h"
#include "logger.h"

volatile sig_atomic_t keep_running = 1;

void signal_handler(int signum) {
    (void)signum; // sinal usado apenas para terminar o loop principal
    keep_running = 0;
}

static void send_503_response(int client_fd, shared_data_t* data, semaphores_t* sems);

/*
 * Cria o socket de escuta na porta dada.
 * Retorna fd >= 0 em sucesso, -1 em erro.
 */
int create_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 128) < 0) {
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}


/*
 * Envia uma resposta HTTP 503 simples e não bloqueante.
 */
static void send_503_response(int client_fd, shared_data_t* data, semaphores_t* sems) {
    const char* body =
        "<html><body><h1>503 Service Unavailable</h1>"
        "<p>Server queue is full, please try again later.</p>"
        "</body></html>";

    size_t body_len = strlen(body);

    send_http_response(
        client_fd,
        503, "Service Unavailable",
        "text/html",
        body,
        body_len,
        0
    );

    // Registar bytes transferidos para este 503 (contamos só o body)
    if (data && sems) {
        stats_record_503(data, sems, body_len);
    }
}


/*
 * Produtor: tenta colocar um client_fd na fila.
 * Usa semáforos (empty_slots, filled_slots, queue_mutex) como bounded buffer.
 * Retorna 0 em sucesso, -1 se falhar (já trata do socket).
 */
int enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd) {
    // Tentar reservar slot livre sem bloquear indefinidamente
    if (sem_trywait(sems->empty_slots) == -1) {
        if (errno == EAGAIN) {
            send_503_response(client_fd, data, sems);
            close(client_fd);
            return -1;
        } else {
            perror("sem_trywait(empty_slots)");
            send_503_response(client_fd, data, sems);
            close(client_fd);
            return -1;
        }
    }

    if (sem_wait(sems->queue_mutex) == -1) {
        perror("sem_wait(queue_mutex)");
        sem_post(sems->empty_slots); // devolve slot
        send_503_response(client_fd, data, sems);
        close(client_fd);
        return -1;
    }

    int capacity = data->queue.capacity > 0 && data->queue.capacity <= MAX_QUEUE_SIZE
                 ? data->queue.capacity
                 : MAX_QUEUE_SIZE;

    if (data->queue.count >= capacity) {
        // Defesa adicional
        sem_post(sems->queue_mutex);
        sem_post(sems->empty_slots);
        send_503_response(client_fd, data, sems);
        close(client_fd);
        return -1;
    }

    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;

    sem_post(sems->queue_mutex);
    sem_post(sems->filled_slots);
    return 0;
}

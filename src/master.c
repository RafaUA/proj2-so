#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#include "shared_mem.h"
#include "semaphores.h"
#include "http.h"      // para send_http_response()
#include "master.h"
#include "config.h"
#include <pthread.h>
#include "worker.h"


volatile sig_atomic_t keep_running = 1;

void signal_handler(int signum) {
    (void)signum; // sinal usado apenas para terminar o loop principal
    keep_running = 0;
}

static void send_503_response(int client_fd);


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
static void send_503_response(int client_fd) {
    const char* body =
        "<html><body><h1>503 Service Unavailable</h1>"
        "<p>Server queue is full, please try again later.</p>"
        "</body></html>";

    send_http_response(
        client_fd,
        503, "Service Unavailable",
        "text/html",
        body,
        strlen(body)
    );
}


/*
 * Produtor: tenta colocar um client_fd na fila.
 * Usa sem_trywait para detectar fila cheia e enviar 503.
 * Retorna 0 em sucesso, -1 se falhar (já trata do socket).
 */
int enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd) {
    // Tentar reservar um slot sem bloquear
    if (sem_trywait(sems->empty_slots) == -1) {
        if (errno == EAGAIN) {
            // Fila cheia -> responde 503 e fecha
            send_503_response(client_fd);
            close(client_fd);
            return -1;
        } else {
            // Erro inesperado no semáforo
            perror("sem_trywait(empty_slots)");
            send_503_response(client_fd);
            close(client_fd);
            return -1;
        }
    }

    // Já está reservado um slot, agora temos que proteger o acesso à fila
    if (sem_wait(sems->queue_mutex) == -1) {
        perror("sem_wait(sems->queue_mutex)");
        // Liberar o slot reservado
        sem_post(sems->empty_slots);
        send_503_response(client_fd);
        close(client_fd);
        return -1;
    }

    // Inserir na fila circular (rear)
    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;

    // Libertar mutex e sinalizar que há mais um item
    sem_post(sems->queue_mutex);
    sem_post(sems->filled_slots);

    return 0;
}


/**
 * main do master:
 *  - lê config
 *  - cria shared memory + semáforos
 *  - cria socket de escuta
 *  - cria pool de worker threads
 *  - loop accept() -> enqueue_connection()
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <ficheiro_config>\n", argv[0]);
        return EXIT_FAILURE;
    }

    server_config_t config;
    if (load_config(argv[1], &config) < 0) {
        perror("load_config");
        return EXIT_FAILURE;
    }

    // Instalar handler para Ctrl+C
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // Criar memória partilhada
    shared_data_t* shared = create_shared_memory();
    if (!shared) {
        perror("create_shared_memory");
        return EXIT_FAILURE;
    }

    // Definir tamanho lógico da queue (respeitando MAX_QUEUE_SIZE)
    int queue_size = config.max_queue_size;
    if (queue_size <= 0 || queue_size > MAX_QUEUE_SIZE) {
        queue_size = MAX_QUEUE_SIZE;
    }

    // Inicializar semáforos
    semaphores_t sems;
    if (init_semaphores(&sems, queue_size) < 0) {
        perror("init_semaphores");
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Criar pool de worker threads (consumidores)
    int total_threads = config.num_workers * config.threads_per_worker;
    if (total_threads <= 0) total_threads = 1; // fallback seguro

    pthread_t* threads = calloc(total_threads, sizeof(pthread_t));
    if (!threads) {
        perror("calloc threads");
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    worker_args_t wargs = {
        .shared = shared,
        .sems = &sems,
        .config = &config
    };

    int threads_created = 0;
    for (int i = 0; i < total_threads; ++i) {
        if (pthread_create(&threads[i], NULL, worker_thread_main, &wargs) != 0) {
            perror("pthread_create");
            threads_created = i;
            keep_running = 0;
            break;
        }
        threads_created++;
    }

    if (threads_created != total_threads) {
        fprintf(stderr, "Erro a criar pool de workers\n");
        for (int i = 0; i < threads_created; ++i) {
            sem_post(sems.filled_slots);
        }
        for (int i = 0; i < threads_created; ++i) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Criar socket de escuta
    int listen_fd = create_server_socket(config.port);
    if (listen_fd < 0) {
        perror("create_server_socket");
        keep_running = 0;
        for (int i = 0; i < threads_created; ++i) {
            sem_post(sems.filled_slots); // acordar threads antes de sair
        }
        for (int i = 0; i < threads_created; ++i) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    printf("Master: a ouvir na porta %d (queue size = %d)\n",
           config.port, queue_size);

    //   LOOP PRINCIPAL (master)
    while (keep_running) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // interrompido por sinal -> verifica keep_running e continua
                continue;
            }
            perror("accept");
            break;
        }

        // Tenta enfileirar na queue partilhada
        if (enqueue_connection(shared, &sems, client_fd) < 0) {
            // Já foi enviada resposta 503 + close() dentro de enqueue_connection
            continue;
        }

        // Se chegámos aqui, uma worker thread nalgum worker process
        // irá fazer dequeue_connection() e tratar o pedido HTTP.
    }

    printf("Master: a terminar e limpar recursos..\n");
    keep_running = 0;

    // Desbloquear threads que possam estar em sem_wait(filled_slots)
    for (int i = 0; i < threads_created; ++i) {
        sem_post(sems.filled_slots);
    }
    for (int i = 0; i < threads_created; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    // Limpeza
    close(listen_fd);
    destroy_semaphores(&sems);
    destroy_shared_memory(shared);

    return EXIT_SUCCESS;
}

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
#include <sys/time.h>

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
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

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
        body_len
    );

    // Registar bytes transferidos para este 503 (contamos só o body)
    if (data && sems) {
        stats_record_503(data, sems, body_len);
    }
}


/*
 * Produtor: tenta colocar um client_fd na fila.
 * Usa mutex/cond para proteger queue e sinalizar consumidores.
 * Retorna 0 em sucesso, -1 se falhar (já trata do socket).
 */
int enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd) {
    if (pthread_mutex_lock(&queue_mutex) != 0) {
        perror("pthread_mutex_lock(queue_mutex)");
        send_503_response(client_fd, data, sems);
        close(client_fd);
        return -1;
    }

    int capacity = data->queue.capacity > 0 && data->queue.capacity <= MAX_QUEUE_SIZE
                 ? data->queue.capacity
                 : MAX_QUEUE_SIZE;

    if (data->queue.count >= capacity) {
        // Fila cheia -> responde 503, regista estatística e fecha
        pthread_mutex_unlock(&queue_mutex);
        send_503_response(client_fd, data, sems);
        close(client_fd);
        return -1;
    }

    // Inserir na fila circular (rear)
    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;

    // Libertar mutex e sinalizar que há mais um item
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);

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
    shared->queue.capacity = queue_size;

    // Inicializar semáforos
    semaphores_t sems;
    if (init_semaphores(&sems, queue_size) < 0) {
        perror("init_semaphores");
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Inicializar cache de ficheiros (10MB por processo)
    if (cache_init(CACHE_DEFAULT_MAX_BYTES) < 0) {
        fprintf(stderr, "Erro a inicializar cache de ficheiros\n");
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Inicializar sistema de logging
    if (logger_init(config.log_file, &sems) < 0) {
        fprintf(stderr, "Erro a inicializar logger\n");
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        close(listen_fd);
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
        keep_running = 0;
        pthread_cond_broadcast(&queue_cond);
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
        pthread_cond_broadcast(&queue_cond);
        for (int i = 0; i < threads_created; ++i) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Timeout curto para accept(), que permite imprimir estatísticas periodicamente
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    // 
    if (setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    printf("Master: a ouvir na porta %d (queue size = %d)\n",
           config.port, queue_size);
    
    time_t start_time = time(NULL);
    time_t last_time_print = start_time;
    //   LOOP PRINCIPAL (master)
    while (keep_running) {
        // A cada 30s imprime estatísticas
        if (time(NULL) - last_time_print >= 30) {
            stats_print(shared, &sems, difftime(time(NULL), start_time));
            last_time_print = time(NULL);
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // interrompido por sinal -> verifica keep_running e continua
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // timeout do accept -> permite cair no print de 30s
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

    // Desbloquear threads que possam estar à espera na cond var
    pthread_cond_broadcast(&queue_cond);
    for (int i = 0; i < threads_created; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    // Mostrar estatísticas finais
    stats_print(shared, &sems, difftime(time(NULL), start_time));
    
    // Fechar sistema de logging (flush + close do ficheiro)
    logger_shutdown();

    // Limpeza
    close(listen_fd);
    destroy_semaphores(&sems);
    destroy_shared_memory(shared);
    cache_destroy();

    return EXIT_SUCCESS;
}

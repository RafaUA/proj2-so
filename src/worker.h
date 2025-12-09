#ifndef WORKER_H
#define WORKER_H

#include <pthread.h>
#include "shared_mem.h"
#include "semaphores.h"
#include "config.h"

/**
 * Argumentos passados a cada worker thread.
 */
typedef struct {
    shared_data_t*  shared;   // memória partilhada (queue + stats)
    semaphores_t*   sems;     // semáforos (empty_slots, filled_slots, queue_mutex, ...)
    server_config_t* config;  // config do servidor (document_root, etc) – para uso futuro
} worker_args_t;

/**
 * Dequeue de uma conexão da fila partilhada.
 *
 * Retorna:
 *   >=0  fd do socket de cliente
 *   -1   em erro (não mexe em sockets)
 */
int dequeue_connection(shared_data_t* data, semaphores_t* sems);

/**
 * Função principal de cada worker thread (consumer).
 * É esta função que o thread pool irá lançar em N threads.
 */
void* worker_thread_main(void* arg);

#endif /* WORKER_H */
#ifndef MASTER_H
#define MASTER_H

#include <signal.h>
#include <pthread.h>
#include "shared_mem.h"
#include "semaphores.h"
#include "stats.h"

/**
 * Variável global usada para terminar o loop principal
 * quando se recebe um sinal (ex: SIGINT).
 */
extern volatile sig_atomic_t keep_running;

/**
 * Mutex/cond para sincronizar o acesso à queue entre threads
 * (implementação do thread pool).
 */
extern pthread_mutex_t queue_mutex;
extern pthread_cond_t queue_cond;

/**
 * Handler para sinais (tipicamente SIGINT).
 * No master.c tens a definição:
 *   void signal_handler(int signum) { keep_running = 0; }
 */
void signal_handler(int signum);

/**
 * Cria o socket de escuta na porta dada.
 * Retorna:
 *   >= 0  fd do socket de escuta
 *   -1    em caso de erro
 */
int create_server_socket(int port);

/**
 * Produtor: tenta enfileirar uma nova conexão na
 * fila partilhada (bounded buffer).
 *
 * Parâmetros:
 *   data  - apontador para a memória partilhada (shared_data_t)
 *   sems  - conjunto de semáforos (semaphores_t)
 *   client_fd - socket da conexão aceite por accept()
 *
 * Retorna:
 *   0  em sucesso (socket ficou na fila para um worker tratar)
 *  -1  em erro ou fila cheia (neste caso, a função já envia 503 e fecha o socket)
 */
int enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd);

#endif /* MASTER_H */

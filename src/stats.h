#ifndef STATS_H
#define STATS_H

#include <stddef.h>
#include "shared_mem.h"
#include "semaphores.h"


/**
 * Marca o início de um pedido:
 *  - incrementa active_connections
 */
int stats_request_start(shared_data_t* data, semaphores_t* sems);


/**
 * Marca o fim de um pedido:
 *  - incrementa total_requests
 *  - soma bytes_transferred
 *  - incrementa o contador do status_code (200/404/500/503)
 *  - decrementa active_connections
 *  - soma response_time_sec a total_response_time_sec e incrementa timed_requests
 */
int stats_request_end(shared_data_t* data,
                      semaphores_t* sems,
                      int status_code,
                      size_t bytes_sent,
                      double response_time_sec);


/**
 * Uso específico para 503 gerados pelo master (queue cheia).
 * Aqui não mexemos em active_connections.
 */
int stats_record_503(shared_data_t* data,
                     semaphores_t* sems,
                     size_t bytes_sent);


/**
 * Imprime as estatísticas atuais (usado pelo master).
 * uptime_seconds é passado pelo master para ser mostrado no cabeçalho.
 */
void stats_print(shared_data_t* data, semaphores_t* sems, double uptime_seconds);


/**
 * Regista um acesso ao cache:
 *  - incrementa cache_lookups
 *  - se hit != 0, incrementa também cache_hits
 */
int stats_cache_access(shared_data_t* data,
                       semaphores_t* sems,
                       int hit);


#endif /* STATS_H */

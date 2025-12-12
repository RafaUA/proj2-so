#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>
#include "semaphores.h"


/**
 * Inicializa o sistema de logging.
 *  - path: caminho do ficheiro de log (ex: config->log_file)
 *  - sems: conjunto de semáforos; usamos sems->log_mutex para sincronizar.
 *
 * Retorna 0 em sucesso, -1 em erro.
 */
int logger_init(const char* path, semaphores_t* sems);


/**
 * Regista uma entrada de log em formato Apache-like:
 *
 *  127.0.0.1 - - [10/Nov/2025:13:55:36 +0000] "GET /index.html HTTP/1.1" 200 2048
 *
 * client_fd   : socket do cliente (usado para descobrir IP)
 * method      : "GET"
 * path        : "/index.html"
 * http_ver    : "HTTP/1.1"
 * status_code : 200, 404, 503, etc.
 * bytes_sent  : nº de bytes do body enviados (aproximado)
 */
void logger_log_request(int client_fd,
                        const char* method,
                        const char* path,
                        const char* http_ver,
                        int status_code,
                        size_t bytes_sent);


/**
 * Flush do buffer e fecho do ficheiro de log.
 * Deve ser chamado no shutdown.
 */
void logger_shutdown(void);


#endif /* LOGGER_H */

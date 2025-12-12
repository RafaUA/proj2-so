#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ctype.h>

#include "stats.h"
#include "worker.h"
#include "master.h"   // para keep_running
#include "http.h"
#include "cache.h"
#include "logger.h"


/**
 * Consumer: retira um client_fd da fila usando semáforos (filled/empty + queue_mutex).
 */
int dequeue_connection(shared_data_t* data, semaphores_t* sems) {
    int client_fd;

    // Esperar por item disponível
    while (sem_wait(sems->filled_slots) == -1) {
        if (errno == EINTR) {
            if (!keep_running) return -1;
            continue;
        }   
        perror("sem_wait(filled_slots)");
        return -1;
    }

    if (sem_wait(sems->queue_mutex) == -1) {
        perror("sem_wait(queue_mutex)");
        // devolve o filled_slots consumido
        sem_post(sems->filled_slots);
        return -1;
    }

    if (data->queue.count == 0) {
        sem_post(sems->queue_mutex);
        return -1;
    }

    client_fd = data->queue.sockets[data->queue.front];
    data->queue.front = (data->queue.front + 1) % MAX_QUEUE_SIZE;
    data->queue.count--;

    sem_post(sems->queue_mutex);
    sem_post(sems->empty_slots);

    return client_fd;
}


// Retorna o tempo atual em segundos (monotónico)
static double now_monotonic_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// lê o pedido HTTP até encontrar "\r\n\r\n" ou encher o buffer
static ssize_t recv_http_request(int client_fd, char* buf, size_t buf_size) {
    size_t total = 0;
    
    // Loop para ler dados até ter um pedido HTTP completo
    while (total < buf_size - 1) {
        // Tenta receber dados do socket do cliente
        ssize_t n = recv(client_fd, buf + total, buf_size - 1 - total, 0);
        
        // Tratar erros de receção
        if (n < 0) {
            if (errno == EINTR) continue;  // Sinal interrompeu -> tenta novamente
            return -1;
        }
        
        // Se recv retorna 0, o cliente fechou a ligação
        if (n == 0) break;
        
        // Avançar o contador de bytes lidos
        total += (size_t)n;
        buf[total] = '\0';  // Terminar string para poder usar strstr()
        
        // Verificar se já temos o fim do header HTTP ("\r\n\r\n")
        if (strstr(buf, "\r\n\r\n")) {
            break;  // Pedido completo recebido
        }
    }
    
    // Se não recebemos nada, erro
    if (total == 0) return -1;
    
    // Garantir que a string está terminada com '\0'
    buf[total] = '\0';
    
    // Retornar número de bytes lidos
    return (ssize_t)total;
}

// procura substring case-insensitive simples
static int contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        while (p[i] && i < nlen && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

/**
 * Constrói o caminho completo do ficheiro a servir, baseado na raiz do documento e no caminho do pedido HTTP.
 * Exemplo: raiz = "www", pedido = "/index.html" -> resultado = "www/index.html"
 * Retorna 0 em sucesso, -1 em erro (ex: buffer insuficiente).
 */
static int build_full_path(const worker_args_t* args, const char* req_path, char* out, size_t out_sz) {
    if (!args || !req_path || !out) return -1;
    
    // Prevenir path traversal simples (ex: "../../etc/passwd")
    if (strstr(req_path, "..")) return -1;

    // Obter a raiz do documento (diretório base dos ficheiros)
    // Se não estiver configurada, usar "www" como padrão
    const char* root = (args->config && args->config->document_root[0] != '\0')
        ? args->config->document_root
        : "www";

    // Obter o caminho relativo do pedido HTTP
    const char* subpath = req_path;
    
    // Remover a "/" inicial (ex: "/index.html" -> "index.html")
    if (*subpath == '/') subpath++;
    
    // Se o caminho está vazio (pedido para "/"), usar "index.html" como padrão
    if (*subpath == '\0') subpath = "index.html";

    // Construir o caminho completo: raiz + "/" + subpath
    // Exemplo: "www" + "/" + "index.html" = "www/index.html"
    int n = snprintf(out, out_sz, "%s/%s", root, subpath);
    
    // Verificar se houve erro ou se o caminho não cabe no buffer
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    
    return 0;
}

static void handle_client_connection(int client_fd, worker_args_t* args) {
    // Configurar timeout de socket por ligação (aplica-se a cada recv)
    // Evita que uma thread fique eternamente à espera de um novo request da mesma ligação
    int timeout_sec = (args->config && args->config->timeout_seconds > 0) ? args->config->timeout_seconds : 30;
    struct timeval tv = {.tv_sec = timeout_sec, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int keep_alive = 1;

    while (keep_running && keep_alive) {
        // Buffer para armazenar o pedido HTTP recebido
        char req_buf[8192]; // 8KB deve ser suficiente para headers
        // Lê o pedido do socket até encontrar o fim dos headers
        ssize_t rlen = recv_http_request(client_fd, req_buf, sizeof(req_buf));
        if (rlen <= 0) {
            // Cliente fechou ou erro de leitura -> terminar ligação sem contabilizar novo pedido
            break;
        }

        double start_time = now_monotonic_sec();

        // Regista pedido em processamento (um por request)
        stats_request_start(args->shared, args->sems);

        // Valores por omissão para o resultado do handler
        int status_code = 500;
        size_t bytes_sent = 0;
        char* file_data = NULL;
        size_t file_size = 0;
        int from_cache = 0;
        int cache_hit = 0;
        int request_ok = 0; // 1 se parse GET válido

        // Estrutura para guardar método, caminho e versão
        http_request_t req;
        // Só aceitamos pedidos GET bem formatados
        if (parse_http_request(req_buf, &req) < 0) {
            const char* body = "<html><body><h1>400 Bad Request</h1></body></html>";
            bytes_sent = strlen(body);
            status_code = 400;
            keep_alive = 0;
            send_http_response(client_fd, status_code, "Bad Request", "text/html", body, bytes_sent, keep_alive);
            goto finish_request;
        }
        request_ok = 1;

        // Determinar se a conexão fica aberta
        int want_close = 0;
        if (contains_ci(req_buf, "connection: close")) {
            want_close = 1;
        } else if (contains_ci(req_buf, "connection: keep-alive")) {
            want_close = 0;
        } else {
            if (strcmp(req.version, "HTTP/1.0") == 0) {     // HTTP/1.0 fecha por omissão
                want_close = 1;
            }
        }
        keep_alive = want_close ? 0 : 1;

        if (strcmp(req.method, "GET") != 0) {
            const char* body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
            bytes_sent = strlen(body);
            status_code = 405;
            keep_alive = 0; // métodos não suportados: fechamos
            send_http_response(client_fd, status_code, "Method Not Allowed", "text/html", body, bytes_sent, keep_alive);
            goto finish_request;
        }

        // Construir o caminho absoluto do ficheiro a servir
        char full_path[1024];
        if (build_full_path(args, req.path, full_path, sizeof(full_path)) != 0) {
            const char* body = "<html><body><h1>400 Bad Request</h1></body></html>";
            bytes_sent = strlen(body);
            status_code = 400;
            keep_alive = 0;
            send_http_response(client_fd, status_code, "Bad Request", "text/html", body, bytes_sent, keep_alive);
            goto finish_request;
        }

        // Tenta obter o ficheiro do cache; se não existir, lê do disco e insere se couber
        if (cache_get_file(full_path, &file_data, &file_size, &from_cache, &cache_hit) != 0) {
            stats_cache_access(args->shared, args->sems, 0); // miss
            const char* body = "<html><body><h1>404 Not Found</h1></body></html>";
            bytes_sent = strlen(body);
            status_code = 404;
            keep_alive = 0; // fechamos em erro
            send_http_response(client_fd, status_code, "Not Found", "text/html", body, bytes_sent, keep_alive);
            goto finish_request;
        }

        // Contabilizar hit/miss de cache
        stats_cache_access(args->shared, args->sems, cache_hit);

        // Detectar e processar Range header
        static __thread char range_value[256];
        range_request_t range;
        int has_range_header = 0;

        // Procurar header "Range:" no pedido
        const char* range_start = strstr(req_buf, "Range:");
        if (!range_start) {
            range_start = strstr(req_buf, "range:");
        }

        if (range_start) {
            // Encontrar o fim da linha do header
            const char* range_end = strstr(range_start, "\r\n");
            if (range_end) {
                // Extrair valor após "Range: " ou "range: "
                const char* value_start = strchr(range_start, ':');
                if (value_start) {
                    value_start++;
                    while (*value_start == ' ') value_start++;

                    size_t value_len = range_end - value_start;
                    if (value_len < sizeof(range_value)) {
                        strncpy(range_value, value_start, value_len);
                        range_value[value_len] = '\0';
                        has_range_header = 1;
                    }
                }
            }
        }

        if (has_range_header) {
            // Validar o range
            if (parse_range_header(range_value, &range, file_size) == 0 && range.has_range) {
                // Range válido - enviar 206 Partial Content
                send_http_response_range(
                    client_fd,
                    "application/octet-stream",
                    file_data,
                    file_size,
                    range.start,
                    range.end,
                    keep_alive
                );
                status_code = 206;
                bytes_sent = range.end - range.start + 1;
            } else {
                // Range inválido - enviar 416 Range Not Satisfiable
                char error_body[256];
                int error_len = snprintf(error_body, sizeof(error_body),
                    "<html><body><h1>416 Range Not Satisfiable</h1></body></html>");
                bytes_sent = error_len;
                status_code = 416;
                keep_alive = 0;
                send_http_response(client_fd, status_code, "Range Not Satisfiable",
                    "text/html", error_body, bytes_sent, keep_alive);
            }
        } else {
            // Sem Range header - comportamento normal
            send_http_response(
                client_fd,
                200, "OK",
                "application/octet-stream",
                file_data,
                file_size,
                keep_alive
            );
            status_code = 200;
            bytes_sent = file_size;
        }

finish_request:
        {
            // Calcula tempo total de resposta e regista stats
            double response_time = now_monotonic_sec() - start_time;
            stats_request_end(
                args->shared,
                args->sems,
                status_code,
                bytes_sent,
                response_time
            );
        }

        // Logging em formato combinado simples
        const char* log_method = request_ok ? req.method : "-";
        const char* log_path   = request_ok ? req.path   : "-";
        const char* log_ver    = request_ok ? req.version: "HTTP/1.1";
        logger_log_request(client_fd, log_method, log_path, log_ver, status_code, bytes_sent);

        // Se não veio do cache, libertar o buffer alocado pelo disco
        if (!from_cache && file_data) {
            free(file_data);
        }

        if (!keep_alive) {
            break;
        }
    }

    // Fecha a ligação ao cliente
    close(client_fd);
}


/**
 * Função que cada worker thread executa.
 *
 * Pseudo-código:
 *   while (keep_running) {
 *       fd = dequeue_connection(...)
 *       se fd < 0 -> continua
 *       handle_client_connection(fd, ...)
 *   }
 */
void* worker_thread_main(void* arg) {
    worker_args_t* wargs = (worker_args_t*)arg;

    while (keep_running) {
        int client_fd = dequeue_connection(wargs->shared, wargs->sems);
        if (client_fd < 0) {
            // Erro ou interrupção; se estamos a terminar, saímos do loop
            if (!keep_running) {
                break;
            }
            continue;
        }

        // Tratar a ligação
        handle_client_connection(client_fd, wargs);
    }

    return NULL;
}    

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>

#include "stats.h"
#include "worker.h"
#include "master.h"   // para keep_running
#include "http.h"
#include "cache.h"
#include "logger.h"


/**
 * Consumer: retira um client_fd da fila.
 * Usa mutex + condvar (queue_mutex/queue_cond) para bloquear até haver trabalho.
 */
int dequeue_connection(shared_data_t* data, semaphores_t* sems) {
    (void)sems; // thread pool usa mutex/cond locais

    int client_fd;

    if (pthread_mutex_lock(&queue_mutex) != 0) {
        perror("pthread_mutex_lock(queue_mutex)");
        return -1;
    }

    while (keep_running && data->queue.count == 0) {
        // bloqueia até haver trabalho ou shutdown
        pthread_cond_wait(&queue_cond, &queue_mutex);
    }

    if (!keep_running) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    }

    // Retirar da fila circular (front)
    client_fd = data->queue.sockets[data->queue.front];
    data->queue.front = (data->queue.front + 1) % MAX_QUEUE_SIZE;       // O % MAX_QUEUE_SIZE garante wrap-around (quando chega ao fim do array volta ao início), de forma a obter uma fila circular.
    data->queue.count--;

    pthread_mutex_unlock(&queue_mutex);

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
    // sleep(5);    // Simula atraso no processamento, para testes de fila cheia (503)
    // Início da medição do tempo de resposta
    double start_time = now_monotonic_sec();
    // Regista pedido em processamento
    stats_request_start(args->shared, args->sems);

    // Valores por omissão para o resultado do handler
    int status_code = 500;
    size_t bytes_sent = 0;
    char* file_data = NULL;
    size_t file_size = 0;
    int from_cache = 0;
    int cache_hit = 0;

    // Buffer para armazenar o pedido HTTP recebido
    char req_buf[8192];
    // Lê o pedido do socket até encontrar o fim dos headers
    ssize_t rlen = recv_http_request(client_fd, req_buf, sizeof(req_buf));
    if (rlen < 0) {
        // Pedido inválido -> responde 400
        const char* body = "<html><body><h1>400 Bad Request</h1></body></html>";
        bytes_sent = strlen(body);
        status_code = 400;
        send_http_response(client_fd, status_code, "Bad Request", "text/html", body, bytes_sent);
        goto finish;
    }

    // Estrutura para guardar método, caminho e versão
    http_request_t req;
    // Só aceitamos pedidos GET bem formatados
    if (parse_http_request(req_buf, &req) < 0 || strcmp(req.method, "GET") != 0) {
        const char* body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
        bytes_sent = strlen(body);
        status_code = 405;
        send_http_response(client_fd, status_code, "Method Not Allowed", "text/html", body, bytes_sent);
        goto finish;
    }

    // Construir o caminho absoluto do ficheiro a servir
    char full_path[1024];
    if (build_full_path(args, req.path, full_path, sizeof(full_path)) != 0) {
        const char* body = "<html><body><h1>400 Bad Request</h1></body></html>";
        bytes_sent = strlen(body);
        status_code = 400;
        send_http_response(client_fd, status_code, "Bad Request", "text/html", body, bytes_sent);
        goto finish;
    }

    // Tenta obter o ficheiro do cache; se não existir, lê do disco e insere se couber
    if (cache_get_file(full_path, &file_data, &file_size, &from_cache, &cache_hit) != 0) {
        stats_cache_access(args->shared, args->sems, 0); // miss
        const char* body = "<html><body><h1>404 Not Found</h1></body></html>";
        bytes_sent = strlen(body);
        status_code = 404;
        send_http_response(client_fd, status_code, "Not Found", "text/html", body, bytes_sent);
        goto finish;
    }

    // Contabilizar hit/miss de cache
    stats_cache_access(args->shared, args->sems, cache_hit);

    // Envia o conteúdo do ficheiro ao cliente (usa buffer do cache ou lido do disco)
    send_http_response(
        client_fd,
        200, "OK",
        "application/octet-stream",
        file_data,
        file_size
    );
    status_code = 200;
    bytes_sent = file_size;

finish:
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

    // Se não veio do cache, libertar o buffer alocado pelo disco
    if (!from_cache && file_data) {
        free(file_data);
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

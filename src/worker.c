#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "worker.h"
#include "master.h"   // para keep_running
#include "http.h"     // para, no futuro, tratar HTTP (send_http_response, etc)


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


/**
 * handler simples para a ligação.
 * Quando implementares o HTTP, esta função vai:
 *   - ler o pedido
 *   - servir ficheiros / respostas
 *   - atualizar estatísticas, etc.
 */
static void handle_client_connection(int client_fd, worker_args_t* args) {
    (void)args;  // evitar warning de unused parameter

    // TODO: substituir por lógica HTTP real na Feature 2/3
    // Por agora, podemos simplesmente fechar a ligação
    // ou mandar uma resposta dummy se quiseres testar.
    //
    // Exemplo minimal:
    /*
    const char* body = "<html><body><h1>Hello from worker</h1></body></html>";
    send_http_response(client_fd, 200, "OK", "text/html", body, strlen(body));
    */

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

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "worker.h"
#include "master.h"   // para keep_running
#include "http.h"     // para, no futuro, tratar HTTP (send_http_response, etc)


/**
 * Consumer: retira um client_fd da fila.
 *
 * Usa:
 *   - sem_wait(filled_slots)  -> espera até haver pelo menos 1 ligação
 *   - sem_wait(queue_mutex)   -> protege o acesso à queue
 *   - sem_post(queue_mutex)   -> liberta o acesso
 *   - sem_post(empty_slots)   -> informa que há mais um slot vazio
 */
int dequeue_connection(shared_data_t* data, semaphores_t* sems) {
    int client_fd;

    // Esperar até haver pelo menos 1 item na fila
    if (sem_wait(sems->filled_slots) == -1) {
        // Se fomos interrompidos por sinal, deixamos o caller decidir o que fazer
        if (errno == EINTR) {
            return -1;
        }

        perror("sem_wait(filled_slots)");
        return -1;
    }

    // Proteger acesso à fila
    if (sem_wait(sems->queue_mutex) == -1) {
        perror("sem_wait(queue_mutex)");

        // Não mexemos na queue, por isso devolvemos o “filled_slots” para não corromper o estado
        sem_post(sems->filled_slots);
        return -1;
    }

    // Se fomos acordados para terminar, evitamos retirar um fd inválido
    if (data->queue.count == 0) {
        sem_post(sems->queue_mutex);
        return -1;
    }

    // Retirar da fila circular (front)
    client_fd = data->queue.sockets[data->queue.front];
    data->queue.front = (data->queue.front + 1) % MAX_QUEUE_SIZE;       // O % MAX_QUEUE_SIZE garante wrap-around (quando chega ao fim do array volta ao início), de forma a obter uma fila circular.
    data->queue.count--;

    // Libertar mutex e sinalizar que há mais um slot vazio
    sem_post(sems->queue_mutex);
    sem_post(sems->empty_slots);

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

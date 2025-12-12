#ifndef SHARED_MEM_H
#define SHARED_MEM_H
#define MAX_QUEUE_SIZE 100

typedef struct {
    long   total_requests;          // nº total de pedidos servidos
    long   bytes_transferred;       // bytes do corpo (body) enviados
    long   timed_requests;          // pedidos com tempo medido
    long   status_200;              // contagem de respostas 200
    long   status_206;              // contagem de respostas 206
    long   status_400;              // contagem de respostas 400
    long   status_404;              // contagem de respostas 404
    long   status_405;              // contagem de respostas 405
    long   status_416;              // contagem de respostas 416
    long   status_500;              // contagem de respostas 500
    long   status_503;              // contagem de respostas 503 (queue cheia, etc.)
    long   status_other;            // outros códigos (3xx, 4xx, 5xx não mapeados)
    int    active_connections;      // nº de pedidos em processamento neste momento
    double total_response_time_sec; // soma dos tempos de resposta (segundos)
    long   cache_hits;              // nº de vezes em que o ficheiro veio do cache
    long   cache_lookups;          // nº total de tentativas de usar cache
} server_stats_t;


typedef struct {
    int sockets[MAX_QUEUE_SIZE];
    int front;
    int rear;
    int count;
    int capacity;  // capacidade lógica configurada (<= MAX_QUEUE_SIZE)
} connection_queue_t;


typedef struct {
    connection_queue_t queue;
    server_stats_t stats;
} shared_data_t;


shared_data_t* create_shared_memory();
void destroy_shared_memory(shared_data_t* data);


#endif

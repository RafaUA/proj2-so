#include <stdio.h>
#include <errno.h>
#include <semaphore.h>

#include "stats.h"


static int safe_sem_wait(sem_t* sem) {
    while (1)
    {
        if (sem_wait(sem) == 0) {
            return 0;
        } else if (errno == EINTR) {
            continue; // retry if interrupted
        } else {
            return -1; // other error
        }
    }
}


static void update_status_counter(server_stats_t* st, int status_code){
    switch (status_code)
    {
    case 200: st->status_200++; break;
    case 404: st->status_404++; break;
    case 500: st->status_500++; break;
    case 503: st->status_503++; break;
    default:
        st->status_other++; // contabiliza outros códigos (3xx, 4xx/5xx não mapeados)
        break;
    }
}


int stats_request_start(shared_data_t* data, semaphores_t* sems) {
    if (!data || !sems) return -1;

    if (safe_sem_wait(sems->stats_mutex) == -1) {
        perror("sem_wait(stats_mutex) in stats_request_start");
        return -1;
    }

    data->stats.active_connections++;

    sem_post(sems->stats_mutex);
    return 0;
}


int stats_request_end(shared_data_t* data,
                      semaphores_t* sems,
                      int status_code,
                      size_t bytes_sent,
                      double response_time_sec) 
{
    if (!data || !sems) return -1;

    if (safe_sem_wait(sems->stats_mutex) == -1) {
        perror("sem_wait(stats_mutex) in stats_request_end");
        return -1;
    }

    server_stats_t* st = &data->stats;

    st->total_requests++;
    st->bytes_transferred += (long)bytes_sent;
    update_status_counter(st, status_code);
 
    st->active_connections--;
    // segurança
    if (st->active_connections < 0) {
        st->active_connections = 0; 
    }

    if (response_time_sec > 0.0) {
        st->timed_requests++;
        st->total_response_time_sec += response_time_sec;
    }

    sem_post(sems->stats_mutex);
    return 0;
}


int stats_record_503(shared_data_t* data,
                     semaphores_t* sems,
                     size_t bytes_sent)
{
    if (!data || !sems) return -1;

    if (safe_sem_wait(sems->stats_mutex) == -1) {
        perror("sem_wait(stats_mutex) in stats_record_503");
        return -1;
    }

    server_stats_t* st = &data->stats;

    st->total_requests++;
    st->bytes_transferred += (long)bytes_sent;
    st->status_503++;

    sem_post(sems->stats_mutex);
    return 0;
}


void stats_print(shared_data_t* data, semaphores_t* sems, double uptime_seconds) {
    if (!data || !sems) return;

    if (safe_sem_wait(sems->stats_mutex) == -1) {
        perror("sem_wait(stats_mutex) in stats_print");
        return;
    }

    server_stats_t cpy = data->stats;   // cópia local para minimizar tempo com mutex

    sem_post(sems->stats_mutex);

    // Average response time
    double avg_response_time = 0.0;
    if (cpy.timed_requests > 0 && cpy.total_response_time_sec > 0.0) {
        avg_response_time = (cpy.total_response_time_sec / (double)cpy.timed_requests) * 1000.0; // em ms
    }

    long successful_2xx = cpy.status_200;
    long client_4xx = cpy.status_404; // só 404 contado até agora
    long server_5xx = cpy.status_500 + cpy.status_503;
    double cache_hit_rate = 0.0; // ainda não há cache implementado

    printf("========================================\n");
    printf("SERVER STATISTICS\n");
    printf("========================================\n");
    printf("Uptime: %.0f seconds\n", uptime_seconds);
    printf("Total Requests: %ld\n", cpy.total_requests);
    printf("Successful (2xx): %ld\n", successful_2xx);
    printf("Client Errors (4xx): %ld\n", client_4xx);
    printf("Server Errors (5xx): %ld\n", server_5xx);
    printf("Bytes Transferred: %ld\n", cpy.bytes_transferred);
    printf("Average Response Time: %.1f ms\n", avg_response_time);
    printf("Active Connections: %d\n", cpy.active_connections);
    printf("Cache Hit Rate: %.1f%%\n", cache_hit_rate);
    printf("========================================\n");
    fflush(stdout);     // garantir que imprime imediatamente
}

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <errno.h>

#include "logger.h"

#define LOG_BUFFER_SIZE 8192
#define LOG_ROTATE_SIZE (10 * 1024 * 1024)  // 10MB

/* Estado global do logger neste processo */
static FILE*          g_log_fp = NULL;              // ficheiro de log aberto
static char           g_log_path[256];              // caminho do ficheiro de log
static char           g_buffer[LOG_BUFFER_SIZE];    // buffer de log em memória
static size_t         g_buffer_len = 0;             // bytes usados no buffer
static size_t         g_file_size  = 0;             // tamanho atual do ficheiro
static semaphores_t*  g_sems = NULL;                // semáforos usados
static int            g_initialized = 0;            // se o logger foi inicializado


/* Função auxiliar: flush do buffer para o disco (sem mexer no semáforo). */
static void logger_flush_unlocked(void) {
    if (!g_log_fp || g_buffer_len == 0) return;

    size_t written = fwrite(g_buffer, 1, g_buffer_len, g_log_fp);
    if (written == g_buffer_len) {
        g_file_size += written;
        g_buffer_len = 0;
        fflush(g_log_fp);
    } else {
        /* Em caso de erro, tentamos pelo menos limpar o buffer em memória. */
        g_buffer_len = 0;
        fflush(g_log_fp);
    }
}

/* Roda o log: fecha o atual, renomeia com timestamp, abre novo vazio. */
static void logger_rotate_unlocked(void) {
    if (!g_log_fp) return;

    logger_flush_unlocked();
    fclose(g_log_fp);
    g_log_fp = NULL;

    char rotated[300];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    /* Formato: <origem>.<YYYY>-<MM>-<DD>-<HH>-<mm>-<SS>
       Ex.: server.log.2025-12-12-10-30-45 */
    snprintf(rotated, sizeof(rotated), "%s.%04d-%02d-%02d-%02d-%02d-%02d",
             g_log_path,
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec);
    rename(g_log_path, rotated);

    g_log_fp = fopen(g_log_path, "a");
    if (!g_log_fp) {
        perror("logger_rotate_unlocked: fopen");
        return;
    }

    g_file_size = 0;
}

/* Descobre IP do cliente a partir do socket. */
static void get_client_ip(int client_fd, char* buf, size_t buflen) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(client_fd, (struct sockaddr*)&addr, &len) == -1) {
        strncpy(buf, "127.0.0.1", buflen);
        buf[buflen - 1] = '\0';
        return;
    }

    void* src = NULL;
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*)&addr;
        src = &s->sin_addr;
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6* s6 = (struct sockaddr_in6*)&addr;
        src = &s6->sin6_addr;
    }

    if (!src || !inet_ntop(addr.ss_family, src, buf, buflen)) {
        strncpy(buf, "127.0.0.1", buflen);
        buf[buflen - 1] = '\0';
    }
}

/* Cria string de timestamp no formato [10/Nov/2025:13:55:36 +0000] */
static void format_time(char* buf, size_t buflen) {
    time_t now = time(NULL);
    struct tm tm_info;

    localtime_r(&now, &tm_info);
    /* %d/%b/%Y:%H:%M:%S %z -> ex: 10/Nov/2025:13:55:36 +0000 */
    strftime(buf, buflen, "%d/%b/%Y:%H:%M:%S %z", &tm_info);
}

/* Inicializa o logger global: define caminho, abre ficheiro, determina tamanho atual e marca o logger como pronto */
int logger_init(const char* path, semaphores_t* sems) {
    if (!path || !sems) {
        errno = EINVAL;     // Argumento inválido
        return -1;
    }

    strncpy(g_log_path, path, sizeof(g_log_path) - 1);
    g_log_path[sizeof(g_log_path) - 1] = '\0';
    g_sems = sems;

    g_log_fp = fopen(g_log_path, "a");      // abrir em modo append
    if (!g_log_fp) {
        perror("logger_init: fopen");
        return -1;
    }

    /* Descobrir tamanho atual do ficheiro para sabermos quando rodar */
    struct stat st;
    if (stat(g_log_path, &st) == 0) {
        g_file_size = (size_t)st.st_size;
    } else {
        g_file_size = 0;
    }

    g_buffer_len = 0;
    g_initialized = 1;
    return 0;
}


void logger_log_request(int client_fd,
                        const char* method,
                        const char* path,
                        const char* http_ver,
                        int status_code,
                        size_t bytes_sent)
{
    if (!g_initialized || !g_sems) return;

    /* Proteger todo o bloco com o semáforo de log */
    if (sem_wait(&g_sems->log_mutex) == -1) {
        perror("logger_log_request: sem_wait(log_mutex)");
        return;
    }

    char ip[64];
    get_client_ip(client_fd, ip, sizeof(ip));

    char tbuf[64];
    format_time(tbuf, sizeof(tbuf));

    char entry[1024];
    int n = snprintf(entry, sizeof(entry),
                     "%s - - [%s] \"%s %s %s\" %d %zu\n",
                     ip,
                     tbuf,
                     method ? method : "-",
                     path ? path : "-",
                     http_ver ? http_ver : "HTTP/1.1",
                     status_code,
                     bytes_sent);

    // Se snprintf falhar ou a entrada for demasiado grande, para aqui
    if (n <= 0) {
        sem_post(&g_sems->log_mutex);
        return;
    }

    size_t entry_len = (size_t)n;

    /* Rodar o log se exceder 10MB (considerando o buffer + entrada) */
    if (g_file_size + g_buffer_len + entry_len > LOG_ROTATE_SIZE) {
        logger_rotate_unlocked();
    }

    /* Se não couber no buffer, flush primeiro */
    if (entry_len > sizeof(g_buffer)) {
        /* Linha gigante improvável -> faz flush do buffer e escreve direto */
        logger_flush_unlocked();
        size_t written = fwrite(entry, 1, entry_len, g_log_fp);
        if (written == entry_len) {
            g_file_size += written;
            fflush(g_log_fp);
        }
    } else {
        // se a entrada cabe no espaço livre atual do buffer
        if (g_buffer_len + entry_len > sizeof(g_buffer)) {
            logger_flush_unlocked();
        }

        // Adiciona ao buffer
        memcpy(g_buffer + g_buffer_len, entry, entry_len);
        g_buffer_len += entry_len;

        /* Política simples: se buffer metade cheio, faz flush */
        if (g_buffer_len >= LOG_BUFFER_SIZE / 2) {
            logger_flush_unlocked();
        }
    }

    sem_post(&g_sems->log_mutex);
}


void logger_shutdown(void) {
    if (!g_initialized) return;

    if (sem_wait(&g_sems->log_mutex) == 0) {
        logger_flush_unlocked();
        if (g_log_fp) {
            fclose(g_log_fp);
            g_log_fp = NULL;
        }
        sem_post(&g_sems->log_mutex);
    }

    g_initialized = 0;
}

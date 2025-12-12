#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "master.h"
#include "shared_mem.h"
#include "semaphores.h"
#include "config.h"
#include "worker.h"
#include "stats.h"
#include "cache.h"
#include "logger.h"

typedef struct {
    const char* config_path;   // NULL -> usar default "server.conf"
    int port_override;         // <=0 -> sem override
    int workers_override;      // <=0 -> sem override
    int threads_override;      // <=0 -> sem override
    int daemon_mode;           // 0/1
    int verbose;               // 0/1
} cmdline_opts_t;

static void print_usage(const char* progname) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  -c, --config PATH   Configuration file path (default: ./server.conf)\n"
        "  -p, --port PORT     Port to listen on (default: 8080 or config file)\n"
        "  -w, --workers NUM   Number of worker threads/processes (override config)\n"
        "  -t, --threads NUM   Threads per worker (override config)\n"
        "  -d, --daemon        Run in background\n"
        "  -v, --verbose       Enable verbose logging\n"
        "  -h, --help          Show this help message\n"
        "      --version       Show version information\n",
        progname
    );
}

static void print_version(void) {
    fprintf(stderr, "webserver 1.0 (SO-2526 Concurrent HTTP Server)\n");
}

static void parse_cmdline(int argc, char* argv[], cmdline_opts_t* opts) {
    static struct option long_opts[] = {
        { "config",  required_argument, NULL, 'c' },
        { "port",    required_argument, NULL, 'p' },
        { "workers", required_argument, NULL, 'w' },
        { "threads", required_argument, NULL, 't' },
        { "daemon",  no_argument,       NULL, 'd' },
        { "verbose", no_argument,       NULL, 'v' },
        { "help",    no_argument,       NULL, 'h' },
        { "version", no_argument,       NULL,  1  },
        { NULL,      0,                 NULL,  0  }
    };

    opts->config_path = NULL;
    opts->port_override = -1;
    opts->workers_override = -1;
    opts->threads_override = -1;
    opts->daemon_mode = 0;
    opts->verbose = 0;

    int c;
    while ((c = getopt_long(argc, argv, "c:p:w:t:dvh", long_opts, NULL)) != -1) {
        switch (c) {
        case 'c':
            opts->config_path = optarg;
            break;
        case 'p':
            opts->port_override = atoi(optarg);
            if (opts->port_override <= 0) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            break;
        case 'w':
            opts->workers_override = atoi(optarg);
            if (opts->workers_override <= 0) {
                fprintf(stderr, "Invalid workers: %s\n", optarg);
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            break;
        case 't':
            opts->threads_override = atoi(optarg);
            if (opts->threads_override <= 0) {
                fprintf(stderr, "Invalid threads: %s\n", optarg);
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            break;
        case 'd':
            opts->daemon_mode = 1;
            break;
        case 'v':
            opts->verbose = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        case 1: // --version
            print_version();
            exit(EXIT_SUCCESS);
        default:
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    // argumento solto após opções -> tratar como path de config (modo antigo)
    if (!opts->config_path && optind < argc) {
        opts->config_path = argv[optind];
    }
}

int main(int argc, char* argv[]) {
    cmdline_opts_t opts;
    parse_cmdline(argc, argv, &opts);

    const char* cfg_path = opts.config_path ? opts.config_path : "server.conf";

    server_config_t config;
    if (load_config(cfg_path, &config) < 0) {
        if (opts.verbose) {
            fprintf(stderr, "Warning: could not open config file '%s', using defaults.\n", cfg_path);
        }
    }

    // Aplicar overrides da linha de comandos
    if (opts.port_override > 0) config.port = opts.port_override;
    if (opts.workers_override > 0) config.num_workers = opts.workers_override;
    if (opts.threads_override > 0) config.threads_per_worker = opts.threads_override;

    if (opts.verbose) {
        fprintf(stderr,
            "Config: port=%d, workers=%d, threads=%d, queue=%d, doc_root=%s, log_file=%s\n",
            config.port,
            config.num_workers,
            config.threads_per_worker,
            config.max_queue_size,
            config.document_root,
            config.log_file);
    }

    if (opts.daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid > 0) {
            // processo pai termina
            exit(EXIT_SUCCESS);
        }

        if (setsid() < 0) {
            perror("setsid");
            exit(EXIT_FAILURE);
        }

        // Redirecionar stdio para /dev/null
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }

    // Instalar handler para Ctrl+C
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    // Ignorar SIGPIPE para evitar terminar processo ao escrever em sockets fechados
    signal(SIGPIPE, SIG_IGN);

    // Criar memória partilhada
    shared_data_t* shared = create_shared_memory();
    if (!shared) {
        perror("create_shared_memory");
        return EXIT_FAILURE;
    }

    // Definir tamanho lógico da queue (respeitando MAX_QUEUE_SIZE)
    int queue_size = config.max_queue_size;
    if (queue_size <= 0 || queue_size > MAX_QUEUE_SIZE) {
        queue_size = MAX_QUEUE_SIZE;
    }
    shared->queue.capacity = queue_size;

    // Inicializar semáforos
    semaphores_t sems;
    if (init_semaphores(&sems, queue_size) < 0) {
        perror("init_semaphores");
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Inicializar cache de ficheiros com tamanho da config (MB -> bytes)
    long cache_bytes = (config.cache_size_mb > 0) ? (long)config.cache_size_mb * 1024L * 1024L
                                                  : CACHE_DEFAULT_MAX_BYTES;
    if (cache_init(cache_bytes) < 0) {
        fprintf(stderr, "Erro a inicializar cache de ficheiros\n");
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Inicializar sistema de logging
    if (logger_init(config.log_file, &sems) < 0) {
        fprintf(stderr, "Erro a inicializar logger\n");
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        cache_destroy();
        return EXIT_FAILURE;
    }

    // Criar pool de worker threads (consumidores)
    int total_threads = config.num_workers * config.threads_per_worker;
    if (total_threads <= 0) total_threads = 1; // fallback seguro

    pthread_t* threads = calloc(total_threads, sizeof(pthread_t));
    if (!threads) {
        perror("calloc threads");
        logger_shutdown();
        cache_destroy();
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    worker_args_t wargs = {
        .shared = shared,
        .sems = &sems,
        .config = &config
    };

    int threads_created = 0;
    for (int i = 0; i < total_threads; ++i) {
        if (pthread_create(&threads[i], NULL, worker_thread_main, &wargs) != 0) {
            perror("pthread_create");
            threads_created = i;
            keep_running = 0;
            break;
        }
        threads_created++;
    }

    if (threads_created != total_threads) {
        fprintf(stderr, "Erro a criar pool de workers\n");
        keep_running = 0;
        for (int i = 0; i < threads_created; ++i) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        logger_shutdown();
        cache_destroy();
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Criar socket de escuta
    int listen_fd = create_server_socket(config.port);
    if (listen_fd < 0) {
        perror("create_server_socket");
        keep_running = 0;
        for (int i = 0; i < threads_created; ++i) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        logger_shutdown();
        cache_destroy();
        destroy_semaphores(&sems);
        destroy_shared_memory(shared);
        return EXIT_FAILURE;
    }

    // Timeout configurável para accept() (TIMEOUT_SECONDS) para evitar bloqueio indefinido
    struct timeval tv;
    tv.tv_sec = (config.timeout_seconds > 0) ? config.timeout_seconds : 1;
    tv.tv_usec = 0;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
    }

    printf("Master: a ouvir na porta %d (queue size = %d)\n",
           config.port, queue_size);
    
    time_t start_time = time(NULL);
    time_t last_time_print = start_time;
    //   LOOP PRINCIPAL (master)
    while (keep_running) {
        // A cada 30s imprime estatísticas
        if (time(NULL) - last_time_print >= 30) {
            stats_print(shared, &sems, difftime(time(NULL), start_time));
            last_time_print = time(NULL);
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                // interrompido por sinal -> verifica keep_running e continua
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // timeout do accept -> permite cair no print de 30s
                continue;
            }
            perror("accept");
            break;
        }

        // Tenta enfileirar na queue partilhada
        if (enqueue_connection(shared, &sems, client_fd) < 0) {
            // Já foi enviada resposta 503 + close() dentro de enqueue_connection
            continue;
        }

        // Se chegámos aqui, uma worker thread nalgum worker process
        // irá fazer dequeue_connection() e tratar o pedido HTTP.
    }

    printf("Master: a terminar e limpar recursos..\n");
    keep_running = 0;

    // Desbloquear threads que possam estar em sem_wait(filled_slots)
    for (int i = 0; i < threads_created; ++i) {
        sem_post(sems.filled_slots);
    }
    for (int i = 0; i < threads_created; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    // Mostrar estatísticas finais
    stats_print(shared, &sems, difftime(time(NULL), start_time));
    
    // Fechar sistema de logging (flush + close do ficheiro)
    logger_shutdown();

    // Limpeza
    close(listen_fd);
    destroy_semaphores(&sems);
    destroy_shared_memory(shared);
    cache_destroy();

    return EXIT_SUCCESS;
}

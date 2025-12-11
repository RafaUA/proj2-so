#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "cache.h"

typedef struct cache_entry {
    char* path;                 // caminho completo do ficheiro
    char* data;                 // dados do ficheiro
    size_t size;                // tamanho em bytes
    struct cache_entry* prev;   // mais recente à frente
    struct cache_entry* next;   // mais antigo atrás
} cache_entry_t;

/* Estado global do cache neste processo (1 cache por processo) */
static cache_entry_t* g_head = NULL;                            // MRU (most recently used)
static cache_entry_t* g_tail = NULL;                            // LRU (least recently used)
static size_t g_total_bytes = 0;                                // total de bytes atualmente no cache
static size_t g_max_bytes = CACHE_DEFAULT_MAX_BYTES;            // limite máximo do cache
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;    // lock para proteger o acesso ao cache
static int g_initialized = 0;                                   // indica se o cache foi inicializado


static void lru_move_to_front(cache_entry_t* e) {
    if (!e || g_head == e){
        return; // já está na frente
    }

    // Remove da posição atual
    if (e->prev) {
        e->prev->next = e->next;
    }
    if (e->next) {
        e->next->prev = e->prev;
    }
    if (g_tail == e) {
        g_tail = e->prev;
    }

    // colocar na frente
    e->prev = NULL;
    e->next = g_head;
    if (g_head) {
        g-head->prev = e;
    }
    g_head = e;
    if (!g_tail) {
        g_tail = e;
    }
}


static void lru_insert_to_front(cache_entry_t* e) {
    e->prev = NULL;
    e->next = g_head;
    if (g_head) {
        g_head->prev = e;
    }
    g_head = e;
    if (!g_tail) {
        g_tail = e;
    }
}


static void lru_remove_entry(cache_entry_t* e) {
    if (!e) return;

    if (e->prev) {
        e->prev->next = e->next;
    }
    if (e->next) {
        e->next->prev = e->prev;
    }

    if (g_head == e) {
        g_head = e->next;
    }
    if (g_tail == e) {
        g_tail = e->prev;
    }
}


/* liberar espaço quando o cache está cheio. */
static void lru_evict_tail(void) {
    if (!g_tail) return;
    
    cache_entry_t* tail = g_tail;

    lru_remove_entry(tail);
    g_total_bytes -= tail->size;

    free(tail->path);
    free(tail->data);
    free(tail);
}


/* Procura uma entrada pelo caminho. Espera-se que o lock já esteja adquirido. */
static cache_entry_t* find_entry(const char* full_path)
{
    for (cache_entry_t* e = g_head; e != NULL; e = e->next) {
        if (strcmp(e->path, full_path) == 0) {
            return e;
        }
    }
    return NULL;
}


/**
 * Lê um ficheiro inteiro para um buffer alocado em memória (malloc).
 * 
 * Argumentos:
 *   full_path  - caminho completo do ficheiro a ler
 *   buf_out    - ponteiro para guardar o endereço do buffer alocado
 *   size_out   - ponteiro para guardar o tamanho do ficheiro (em bytes)
 * 
 * Retorna:
 *   0 em sucesso (buffer preenchido e tamanho definido)
 *   -1 em erro (ficheiro não existe, não é leitura, sem memória, etc.)
 */
static int read_file_fully(const char* full_path, char** buf_out, size_t* size_out) {
    // Abrir o ficheiro para leitura (O_RDONLY = read-only)
    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    
    // Obter informações do ficheiro (tamanho, tipo, permissões)
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    // Verificar que é realmente um ficheiro regular (não diretório, symlink, etc.)
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }

    // Obter tamanho do ficheiro
    off_t fsize = st.st_size;
    if (fsize < 0) {
        close(fd);
        errno = EIO;  // erro de I/O
        return -1;
    }

    // Caso especial - ficheiro vazio
    if (fsize == 0) {
        // Ficheiro vazio -> alocar 1 byte (para evitar malloc(0)) e retornar tamanho 0
        char* buf = malloc(1);
        if (!buf) {
            close(fd);
            return -1;
        }
        *buf_out = buf;     // guarda endereço do buffer
        *size_out = 0;      // tamanho real é 0
        close(fd);
        return 0;
    }

    // Alocar buffer com exatamente o tamanho do ficheiro
    char* buf = malloc((size_t)fsize);
    if (!buf) {
        close(fd);
        return -1;
    }

    // Ler o ficheiro em pedaços (loop, porque read() pode ler menos bytes que pedido)
    size_t to_read = (size_t)fsize;        // total de bytes a ler
    size_t total_read = 0;                 // bytes já lidos
    while (total_read < to_read) {
        // Ler até (to_read - total_read) bytes a partir da posição total_read
        ssize_t n = read(fd, buf + total_read, to_read - total_read);
        
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            // Erro real (permissão, disco corrompido, etc.)
            free(buf);
            close(fd);
            return -1;
        }
        
        if (n == 0) {
            break;
        }
        
        // Avançar contador de bytes lidos
        total_read += (size_t)n;
    }

    close(fd);

    *buf_out = buf;           // guarda endereço do buffer alocado
    *size_out = total_read;   // guarda tamanho real lido
    
    return 0;
}


int cache_init(long max_bytes) {
    if (g_max_bytes > 0) {
        g_max_bytes = (size_t)max_bytes;
    } else {
        g_max_bytes = CACHE_DEFAULT_MAX_BYTES;
    }

    g_head = g_tail = NULL;
    g_total_bytes = 0;
    g_initialized = 1;

    return 0;
}


void cache_destroy(void) {
    if (!g_initialized) return;

    pthread_rwlock_wrlock(&g_lock);

    cache_entry_t* e = g_head;
    while (e) {
        cache_entry_t* next = e->next;
        free(e->path);
        free(e->data);
        free(e);
        e = next;
    }

    g_head = g_tail = NULL;
    g_total_bytes = 0;
    g_initialized = 0;

    pthread_rwlock_unlock(&g_lock);
}


/**
 * Lógica:
 *  1. RDLOCK + procura entrada.
 *     - se encontrar => hit (from_cache=1, is_hit=1)
 *  2. se não encontrar => unlock, ler ficheiro do disco.
 *     - se ficheiro > CACHE_MAX_FILE_SIZE => devolve buffer NON-cache (from_cache=0)
 *     - se ficheiro <= CACHE_MAX_FILE_SIZE => WRLOCK, volta a verificar, insere se ainda não existir.
 */
int cache_get_file(const char* full_path,
                   char** data_out,
                   size_t* size_out,
                   int* from_cache_out,
                   int* is_hit_out)
{
    if (!g_initialized || !full_path || !data_out || !size_out) {
        return -1;
    }

    /* Flags de saída: por omissão, assumimos que não veio do cache e foi
       um miss (is_hit = 0). Chamador pode passar NULL se não quiser esses dados. */
    if (from_cache_out) *from_cache_out = 0;
    if (is_hit_out) *is_hit_out = 0;

    /* Tenta encontrar a entrada com lock de leitura (múltiplos leitores permitidos) */
    pthread_rwlock_rdlock(&g_lock);
    cache_entry_t* e = find_entry(full_path);
    if (e) {
        /* Hit: devolvemos o buffer já em memória (não duplicamos) */
        *data_out = e->data;
        *size_out = e->size;
        if (from_cache_out) *from_cache_out = 1;  // veio do cache
        if (is_hit_out) *is_hit_out = 1;          // hit

        pthread_rwlock_unlock(&g_lock);
        return 0;
    }
    /* Não encontrou -> libertar o rdlock e seguir para leitura do disco */
    pthread_rwlock_unlock(&g_lock);

    /* Miss: ler o ficheiro do disco sem segurar o lock do cache (evita bloquear leitores) */
    char* buf = NULL;
    size_t fsize = 0;
    if (read_file_fully(full_path, &buf, &fsize) < 0) {
        return -1;
    }

    /* Se o ficheiro é demasiado grande para o cache, não o inserimos:
       devolvemos apenas o buffer lido (não encapsulado no cache). */
    if (fsize > CACHE_MAX_FILE_SIZE) {
        *data_out = buf;                     // buffer alocado por read_file_fully
        *size_out = fsize;
        if (from_cache_out) *from_cache_out = 0;
        // is_hit_out já está a 0 (miss)
        return 0;
    }

    /* Vamos inserir no cache: obter WRLOCK para exclusividade ao modificar estruturas */
    pthread_rwlock_wrlock(&g_lock);

    /* Entre ler do disco e adquirir o WRLOCK, outro thread pode ter inserido a mesma entrada.
       Re-verificamos para evitar duplicação e desperdício de memória. */
    e = find_entry(full_path);
    if (e) {
        /* Já foi inserida por outro thread -> libertamos o buffer que lemos e usamos a existente */
        free(buf);

        *data_out = e->data;
        *size_out = e->size;
        if (from_cache_out) *from_cache_out = 1;
        if (is_hit_out) *is_hit_out = 1;

        pthread_rwlock_unlock(&g_lock);
        return 0;
    }

    /* Garantir espaço suficiente: remover entradas LRU até caber o novo ficheiro */
    while (g_total_bytes + fsize > g_max_bytes && g_tail != NULL) {
        lru_evict_tail();
    }

    /* Criar nova entrada de cache com os dados lidos */
    cache_entry_t* new_e = malloc(sizeof(cache_entry_t));
    if (!new_e) {
        pthread_rwlock_unlock(&g_lock);
        free(buf);
        return -1;
    }

    new_e->path = strdup(full_path); // copia do caminho (para uso futuro / free)
    if (!new_e->path) {
        free(new_e);
        pthread_rwlock_unlock(&g_lock);
        free(buf);
        return -1;
    }

    /* Transferimos a posse do buffer 'buf' para a nova entrada:
       não devemos free() esse buffer depois desta atribuição (a nova entrada passa a ser dona). */
    new_e->data = buf;
    new_e->size = fsize;
    new_e->prev = new_e->next = NULL;

    /* Inserir a nova entrada na frente (MRU) e atualizar contadores */
    lru_insert_front(new_e);
    g_total_bytes += fsize;

    /* Devolver ao chamador o ponteiro para os dados no cache */
    *data_out = new_e->data;
    *size_out = new_e->size;
    if (from_cache_out) *from_cache_out = 1;
    // is_hit_out mantém-se 0 porque foi miss inicialmente

    pthread_rwlock_unlock(&g_lock);
    return 0;
}

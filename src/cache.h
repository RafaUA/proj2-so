#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

/**
 * Limites da Feature 4:
 *  - só ficheiros < 1MB vão para o cache
 *  - cache total ~10MB por processo
 */
#define CACHE_MAX_FILE_SIZE (1024 * 1024)      // 1MB
#define CACHE_DEFAULT_MAX_BYTES (10 * 1024 * 1024) // 10MB


/**
 * Inicializa o cache global do processo.
 * max_bytes <= 0 => usa CACHE_DEFAULT_MAX_BYTES.
 *
 * Retorna 0 em sucesso, -1 em erro.
 */
int cache_init(long max_bytes);


/**
 * Liberta toda a memória do cache global.
 * Deve ser chamada no shutdown do processo.
 */
void cache_destroy(void);


/**
 * Obtém o conteúdo completo de um ficheiro.
 *
 * full_path       : caminho absoluto (ex: DOCUMENT_ROOT + path do pedido)
 * data_out        : devolve ponteiro para os dados
 * size_out        : devolve tamanho em bytes
 * from_cache_out  : 1 se veio do cache, 0 se foi lido de disco
 * is_hit_out      : 1 se houve *hit* no cache, 0 se foi *miss*
 *
 * NOTA:
 *  - Se from_cache_out == 1, NÃO fazemos free(data_out).
 *  - Se from_cache_out == 0, o chamador é responsável por free(data_out).
 *
 * Retorna 0 em sucesso, -1 em erro (ficheiro não existe, erro de I/O, etc).
 */
int cache_get_file(const char* full_path,
                   char** data_out,
                   size_t* size_out,
                   int* from_cache_out,
                   int* is_hit_out);


#endif /* CACHE_H */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int load_config(const char* filename, server_config_t* config) {
    FILE* fp = fopen(filename, "r");
    if (!fp) return -1;

    // inicializar com defaults razoáveis
    config->port = 8080;
    config->num_workers = 1;
    config->threads_per_worker = 1;
    config->max_queue_size = 100;
    strcpy(config->document_root, "www");

    char line[512], key[128], value[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "%[^=]=%s", key, value) == 2) {
            if (strcmp(key, "PORT") == 0) {
                config->port = atoi(value);

            } else if (strcmp(key, "NUM_WORKERS") == 0) {
                config->num_workers = atoi(value);

            } else if (strcmp(key, "THREADS_PER_WORKER") == 0) {
                config->threads_per_worker = atoi(value);

            } else if (strcmp(key, "DOCUMENT_ROOT") == 0) {
                strncpy(config->document_root, value, sizeof(config->document_root) - 1);
                config->document_root[sizeof(config->document_root) - 1] = '\0';

            } else if (strcmp(key, "MAX_QUEUE_SIZE") == 0) {
                config->max_queue_size = atoi(value);
            }
        }
    }
    fclose(fp);
    return 0;
}

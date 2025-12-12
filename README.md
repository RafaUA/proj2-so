# SO-2526 – Concurrent HTTP Server (TP2)

## 1. Overview

Este projeto implementa um **servidor HTTP concorrente** em C, desenvolvido no âmbito da unidade curricular de Sistemas Operativos (SO-2526).

O servidor suporta:

- Modelo **processo único + pool de threads**;
- **Fila bounded** de conexões em memória partilhada (producer–consumer com semáforos);
- **Thread pool** fixo de workers;
- **Estatísticas** globais agregadas;
- **Cache LRU** de ficheiros;
- **Logging** thread-safe em formato semelhante ao Apache;
- **HTTP Keep-Alive** (ligações persistentes);
- **Range Requests (HTTP 206 Partial Content)**.

O código está organizado em módulos (`main.c`, `master.c`, `worker.c`, `http.c`, `cache.c`, `logger.c`, etc.) com responsabilidades bem separadas.

---

## 2. Features Implementadas

### 2.1. Core Features

1. **Connection Queue (Producer–Consumer)**  
   - Fila circular bounded em memória partilhada (`shared_data_t`), com capacidade configurável até `MAX_QUEUE_SIZE` (típico: 100).
   - Master process (produtor) faz `accept()` e enfileira `client_fd`.
   - Worker threads (consumidores) retiram `client_fd` da fila.
   - Sincronização com semáforos POSIX:
     - `empty_slots`, `filled_slots`, `queue_mutex`.
   - Quando a fila está cheia, o servidor responde com:
     - `503 Service Unavailable` + fecha a ligação.

2. **Thread Pool Management**  
   - Número de workers e threads por worker configurável (`NUM_WORKERS`, `THREADS_PER_WORKER`).
   - Threads criadas no arranque e bloqueadas à espera de trabalho.
   - Cada thread:
     - faz `dequeue_connection`,
     - chama `handle_client_connection` para processar um ou mais pedidos HTTP (Keep-Alive),
     - termina de forma ordeira em shutdown (SIGINT).

3. **Shared Statistics**  
   - Estatísticas globais em `shared_data_t`, protegidas por `stats_mutex`:
     - `Total Requests`,
     - `2xx`, `4xx`, `5xx`,
     - `Bytes Transferred`,
     - `Active Connections`,
     - `Average Response Time`,
     - `Cache Hits` / `Cache Lookups` (hit rate).
   - Master process imprime estatísticas periodicamente (p.ex. a cada 30 s):

     ```text
     ========================================
     SERVER STATISTICS
     ========================================
     Uptime: 120 seconds
     Total Requests: 1,542
     Successful (2xx): 1,425
     Client Errors (4xx): 112
     Server Errors (5xx): 5
     Bytes Transferred: 15,728,640
     Average Response Time: 8.3 ms
     Active Connections: 12
     Cache Hit Rate: 82.4%
     ========================================
     ```

4. **Thread-Safe File Cache (LRU)**  
   - Cache LRU por processo para ficheiros **< 1 MB**.
   - Tamanho máximo configurável (`CACHE_SIZE_MB`, p.ex. 10 MB por processo).
   - Sincronização com `pthread_rwlock_t`:
     - múltiplos leitores em paralelo,
     - escritor exclusivo para inserir/evict/promover entradas.
   - Em `cache_get_file`:
     - se hit: devolve ponteiro para buffer em cache (não fazer `free()`),
     - se miss: lê de disco, insere se couber (respeitando limite), devolve buffer “não-cacheado” (o chamador faz `free()`).

5. **Thread-Safe Logging**  
   - Um único ficheiro de log (configurável via `LOG_FILE`) para todas as threads.
   - Semáforo `log_mutex` garante exclusão mútua.
   - Buffer interno para reduzir chamadas a `write()`.
   - **Log rotation** quando tamanho > 10 MB (ficheiro atual renomeado para `.1`, novo ficheiro aberto).
   - Formato semelhante ao Apache Combined:

     ```text
     127.0.0.1 - - [12/Dec/2025:10:30:08 +0000] "GET /index.html HTTP/1.1" 200 97
     ```

### 2.2. Bónus Implementados

- **HTTP Keep-Alive (Persistent Connections)**  
  - Suporte para múltiplos pedidos HTTP na mesma ligação TCP.
  - Semântica respeitando:
    - `Connection: keep-alive` / `Connection: close`,
    - diferenças entre HTTP/1.0 (fecha por omissão) e HTTP/1.1 (mantém por omissão).
  - Cada pedido é contabilizado separadamente em stats e logging.

- **Range Requests (Partial Content / HTTP 206)**  
  - Suporte a `Range: bytes=start-end`.
  - Resposta `206 Partial Content` com header `Content-Range`.
  - Caso range inválido ou fora do ficheiro, responde `416 Range Not Satisfiable`.
  - Integrado com o cache: o ficheiro completo pode vir do cache; a resposta envia apenas o segmento pedido.

---

## 3. Arquitetura e Módulos

### 3.1. Principais Ficheiros

- `src/main.c`  
  - Ponto de entrada (`main`).
  - Parse da linha de comandos (`getopt_long`).
  - Leitura de `server.conf` com defaults.
  - Daemon mode (`-d`).
  - Inicialização de:
    - memória partilhada (`shared_mem`),
    - semáforos (`semaphores`),
    - cache (`cache_init`),
    - logger (`logger_init`),
    - stats (`stats_init`).
  - Criação do **thread pool**.
  - Loop principal de `accept()` + `enqueue_connection`.
  - Shutdown ordenado (SIGINT, join threads, destroy recursos).

- `src/master.c`  
  - `create_server_socket(port)`: `socket` + `bind` + `listen`.
  - `enqueue_connection(...)`: produtor na fila de conexões.
  - `send_503_response(client_fd)`: envia 503 quando fila cheia.
  - Handler de `SIGINT` (`keep_running = 0`).

- `src/worker.c`  
  - Criação do thread pool.
  - `dequeue_connection(...)`: consumidor da fila.
  - `handle_client_connection(...)`: ciclo por ligação (Keep-Alive + cache + stats + logger + Range).

- `src/http.c / src/http.h`  
  - Parsing de pedidos HTTP (`http_request_t`).
  - `recv_http_request`: leitura de headers.
  - `send_http_response`: montagem de status line, headers (`Content-Length`, `Content-Type`, `Connection`, `Content-Range`, …) e corpo.

- `src/shared_mem.c / src/shared_mem.h`  
  - `shared_data_t`:
    - queue circular de `client_fd`,
    - bloco de estatísticas.
  - Criação/destruição de memória partilhada.

- `src/semaphores.c / src/semaphores.h`  
  - `semaphores_t`:
    - `empty_slots`, `filled_slots`, `queue_mutex`,
    - `stats_mutex`, `log_mutex`.
  - Init/destroy.

- `src/cache.c / src/cache.h`  
  - Cache LRU com lista duplamente ligada.
  - Protegido por `pthread_rwlock_t`.
  - Integração com Range e stats de cache.

- `src/stats.c / src/stats.h`  
  - `stats_request_start`, `stats_request_end`.
  - `stats_cache_access`.
  - `stats_print` (periodicamente pelo master).

- `src/logger.c / src/logger.h`  
  - Logging thread-safe com `log_mutex`.
  - Bufferização + rotação.

- `tests/test_concurrent.c`  
  - Cliente de teste que lança várias threads a fazer GETs simultâneos.

- `tests/test_load.sh` (e/ou `test_load.sh`)  
  - Script de testes funcionais + carga (`curl` + `ab`), incluindo cache timing.

---

## 4. Compilação e Execução

### 4.1. Requisitos

- Compilador C (gcc).
- POSIX threads (`pthread`).
- POSIX semaphores (`sem_open`, `sem_wait`, …).
- (Opcional) ApacheBench (`ab`) para testes de carga:
  ```bash
  sudo apt-get install apache2-utils
  ```

### 4.2. Compilar

Na raiz do projeto:

```bash
make
```

Isto gera o binário:

```bash
./webserver
```

Limpar objetos e binário:

```bash
make clean
```

---

## 5. Execução do Servidor

### 5.1. Opções de Linha de Comandos

O binário suporta:

```
Usage: ./webserver [OPTIONS] [CONFIG_FILE]

Options:
  -c, --config PATH   Caminho para ficheiro de configuração (default: ./server.conf)
  -p, --port PORT     Porto TCP a escutar (default: 8080)
  -w, --workers NUM   Número de worker "groups" (ver config)
  -t, --threads NUM   Threads por worker (default: 10)
  -d, --daemon        Executar em background (daemon)
  -v, --verbose       Ativar logging verboso no arranque
  -h, --help          Mostrar mensagem de ajuda
      --version       Mostrar versão do servidor
```

Também é aceite:

```bash
./webserver server.conf
```

como atalho para:

```bash
./webserver -c server.conf
```

### 5.2. Exemplos

- Configuração por omissão (usa server.conf se existir, senão defaults internos):
  ```bash
  ./webserver
  ```
- Ficheiro de configuração específico:
  ```bash
  ./webserver -c server.conf
  ```
- Porto customizado:
  ```bash
  ./webserver -c server.conf -p 9090
  ```
- Número de threads alterado:
  ```bash
  ./webserver -c server.conf -w 2 -t 4 -v
  ```
- Modo daemon (background):
  ```bash
  ./webserver -c server.conf -d
  ```
- Parar o servidor (foreground): Ctrl+C (SIGINT); se em daemon, enviar kill ao PID.

### 5.3. Target make run

Para conveniência:

```bash
make run
```

equivale a:

```bash
./webserver server.conf
```

---

## 6. Ficheiro de Configuração (server.conf)

Exemplo:

```
PORT=8080
DOCUMENT_ROOT=./www
NUM_WORKERS=1
THREADS_PER_WORKER=4
MAX_QUEUE_SIZE=100
LOG_FILE=access.log
CACHE_SIZE_MB=10
TIMEOUT_SECONDS=30
```

Parâmetros principais:
- PORT - porto de escuta.
- DOCUMENT_ROOT - diretório base de ficheiros estáticos (e.g. ./www).
- NUM_WORKERS - número de grupos de worker (pode ser fator de multiplicação de threads).
- THREADS_PER_WORKER - threads em cada grupo.
- MAX_QUEUE_SIZE - capacidade máxima da fila de conexões.
- LOG_FILE - caminho para o ficheiro de log.
- CACHE_SIZE_MB - tamanho máximo do cache LRU (por processo).
- TIMEOUT_SECONDS - timeout de receção por ligação (SO_RCVTIMEO).

---

## 7. Document Root (www/)

O servidor serve ficheiros a partir de DOCUMENT_ROOT.

Exemplos:
- DOCUMENT_ROOT=./www
- GET / ? ./www/index.html
- GET /index.html ? ./www/index.html
- GET /style.css ? ./www/style.css
- GET /images/logo.png ? ./www/images/logo.png

Exemplo mínimo:

```bash
mkdir -p www
echo "<h1>Olá, SO!</h1>" > www/index.html
echo "body { background: #eee; }" > www/style.css
echo "console.log('ok');" > www/script.js
```

---

## 8. Testes

### 8.1. Testes Funcionais Básicos

Com o servidor a correr (ex.: ./webserver -c server.conf):

```bash
# GET básico
curl http://localhost:8080/index.html

# HEAD
curl -I http://localhost:8080/index.html

# 404 Not Found
curl -v http://localhost:8080/nonexistent.html

# Diferentes tipos de ficheiro (se existirem)
curl http://localhost:8080/style.css
curl http://localhost:8080/script.js
curl http://localhost:8080/image.png
```

### 8.2. Testes de Keep-Alive

```bash
curl -v --http1.1 --header "Connection: keep-alive" http://localhost:8080/index.html
```

Esperado: resposta com Connection: keep-alive e ligação mantida.

### 8.3. Testes de Range (Partial Content)

```bash
# bytes 0-99
curl -v -H "Range: bytes=0-99" http://localhost:8080/index.html

# �ltimos 100 bytes
curl -v -H "Range: bytes=-100" http://localhost:8080/index.html
```

Esperado: HTTP/1.1 206 Partial Content, header Content-Range, corpo com o segmento apropriado.

---

## 9. Testes Automatizados (make test, make perf)

Há um script em tests/test_load.sh que integra:
- testes funcionais com curl,
- testes concorrentes (curl + tests/test_concurrent),
- testes de carga com ab,
- medição de tempos para avaliar o cache.

### 9.1. Executar teste completo

```bash
make test
# ou
make perf
```

Dependendo da versão do script, ele pode arrancar o servidor internamente ou assumir que já está a correr em localhost:8080 (ver comentários no próprio script).

### 9.2. Testes de carga com ApacheBench

```bash
# Carga leve
ab -n 100 -c 10 http://localhost:8080/index.html
# Carga média
ab -n 1000 -c 50 http://localhost:8080/index.html
# Carga pesada
ab -n 10000 -c 100 http://localhost:8080/index.html
# Stress (pode gerar 503 se fila encher)
ab -n 20000 -c 200 http://localhost:8080/
```

---

## 10. Testes de Concorrência e Cache

### 10.1. Concorrência com curl

```bash
# 100 pedidos em paralelo
for i in {1..100}; do
  curl -s --max-time 5 http://localhost:8080/index.html > /dev/null &
done
wait

# 50 pedidos mistos
for i in {1..50}; do
  case $(( i % 3 )) in
    0) path="/index.html" ;;
    1) path="/style.css" ;;
    2) path="/script.js" ;;
  esac
  curl -s --max-time 5 "http://localhost:8080${path}" > /dev/null &
done
wait
```

### 10.2. Cliente concorrente em C

```bash
make tests/test_concurrent
./tests/test_concurrent
```

### 10.3. Avaliar eficácia do cache

```bash
# Primeiro pedido (miss esperado)
time curl -s http://localhost:8080/index.html > /dev/null
# Pedidos subsequentes (hits esperados)
for i in {1..10}; do
  time curl -s http://localhost:8080/index.html > /dev/null
done
```

---

## 11. Valgrind e Helgrind

### 11.1. Memory Leak Check

```bash
make valgrind
```

ou manualmente:

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./webserver -c server.conf
```

Gerar tráfego noutro terminal (por exemplo com ab), depois parar o servidor e analisar o relatório.

### 11.2. Data Race Check (Helgrind)

```bash
make helgrind
```

ou manualmente:

```bash
valgrind --tool=helgrind ./webserver -c server.conf
```

Gerar tráfego concorrente (por exemplo ab -n 5000 -c 100) e analisar o log.

---

## 12. Notas Finais

O servidor cumpre o TP2: uso de semáforos POSIX, threads, memória partilhada, cache, logging, estatísticas e HTTP. Código modular e extensível (CGI, virtual hosts, HTTPS, etc.). Targets test, perf, valgrind, helgrind permitem reproduzir o comportamento sob carga e cenários variados.

---

## Known Issues
### Current Limitations
1. No SSL/TLS Support: HTTP only, no HTTPS (bonus feature)
2. Static Files Only: No CGI or dynamic content (bonus feature)
3. Single Document Root: No virtual hosts (bonus feature)
4. No Real-time Web Dashboard (Statistics Viewer) & WebSocket Support (bonus features)

---

## Authors

### Rafael Semedo
- Student NMec: 115665 
- Email: rafaeljcmsemedo@ua.pt
- GitHub: @RafaUA (https://github.com/RafaUA)

### Felizardo Xavier
- Student NMec: 121527
- Email: felizardo17@ua.pt
- GitHub: @Zardo171817 (https://github.com/Zardo171817)

---

## Acknowledgments

### References
- [setsockopt - POSIX Manual](https://www.man7.org/linux/man-pages/man3/setsockopt.3p.html)
- [How to use struct timeval to get execution time](https://stackoverflow.com/questions/12722904/how-to-use-struct-timeval-to-get-the-execution-time)
- [Difference between EAGAIN or EWOULDBLOCK](https://stackoverflow.com/questions/49049430/difference-between-eagain-or-ewouldblock)
- [ChatGPT - AI Assistant](https://chatgpt.com/)
- [What does EAGAIN mean](https://stackoverflow.com/questions/4058368/what-does-eagain-mean)
- [pthread_rwlock_rdlock - POSIX Manual](https://www.man7.org/linux/man-pages/man3/pthread_rwlock_rdlock.3p.html)
- [clock_gettime - QNX Documentation](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html)
- [open - POSIX Manual](https://www.man7.org/linux/man-pages/man2/open.2.html)
- [What is S_ISREG and what does it do](https://stackoverflow.com/questions/40163270/what-is-s-isreg-and-what-does-it-do)
- [loff_t - POSIX Manual](https://www.man7.org/linux/man-pages/man3/loff_t.3type.html)
- [What does _XOPEN_SOURCE do](https://stackoverflow.com/questions/5378778/what-does-d-xopen-source-do-mean)
- [strdup - OpenGroup Documentation](https://pubs.opengroup.org/onlinepubs/9699919799/functions/strdup.html)
- [C fopen function with examples](https://www.geeksforgeeks.org/c/c-fopen-function-with-examples/)
- [C snprintf function reference](https://www.w3schools.com/c/ref_stdio_snprintf.php)
- [C Date and Time functions](https://www.w3schools.com/c/c_date_time.php)
- [C memcpy function - TutorialsPoint](https://www.tutorialspoint.com/c_standard_library/c_function_memcpy.htm)
### Tools Used
- Compiler: GCC 11.4.0
- Debugger: GDB 12.1, Valgrind 3.19
- Build System: GNU Make 4.3
- Editor: [VS Code / Vim / etc.]
- Version Control: Git 2.34
### Course Information
- Course: Sistemas Operativos (40381-SO)
- Semester: 1º semester 2025/2026
- Instructor: Prof. Pedro Azevedo Fernandes / Prof. Nuno Lau
- Institution: Universidade de Aveio
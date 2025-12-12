# Compilador e flags
CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g -pthread

# Diretório das sources
SRC_DIR = src

# Nome do executável
TARGET  = webserver

# Ficheiros fonte
SRCS    = $(SRC_DIR)/master.c \
          $(SRC_DIR)/worker.c \
          $(SRC_DIR)/main.c \
          $(SRC_DIR)/shared_mem.c \
          $(SRC_DIR)/semaphores.c \
          $(SRC_DIR)/http.c \
          $(SRC_DIR)/config.c \
		  ${SRC_DIR}/stats.c \
		  ${SRC_DIR}/cache.c \
		  ${SRC_DIR}/logger.c

# Objetos gerados (ficam também em src/)
OBJS    = $(SRCS:.c=.o)

# Target por omissão
all: $(TARGET)

# Link final
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Regra genérica para compilar .c -> .o dentro de src/
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Limpar objetos e binário
clean:
	rm -f $(OBJS) $(TARGET)

# Limpar tudo + ficheiros temporários comuns
distclean: clean
	rm -f *~ core

# Correr o servidor com o ficheiro de config
run: $(TARGET)
	./$(TARGET) server.conf

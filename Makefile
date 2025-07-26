CC = gcc
CFLAGS = -Wall -Wextra -Werror -I./src
LDFLAGS = -lyaml -luuid

SRC_DIR = src
SRCS_COMMON = $(SRC_DIR)/distZIP.c
OBJS_COMMON = $(SRCS_COMMON:.c=.o)

CLIENT_OBJ = $(SRC_DIR)/client.o
SERVER_OBJ = $(SRC_DIR)/server.o

all: client server

client: $(CLIENT_OBJ) $(OBJS_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS)

server: $(SERVER_OBJ) $(OBJS_COMMON)
	$(CC) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRC_DIR)/*.o client server

.PHONY: all clean


CC = gcc
CFLAGS = -Wall -Wextra -Werror -I./src
LDFLAGS = -lyaml -luuid -lpthread -lz

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

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/distZIP.h $(SRC_DIR)/protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRC_DIR)/*.o client server

install: all
	install -d /usr/local/bin
	install -m 755 client /usr/local/bin/distzip-client
	install -m 755 server /usr/local/bin/distzip-server

.PHONY: all clean install

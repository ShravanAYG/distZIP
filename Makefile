CFLAGS=-Wall -Wextra -Werror
LDFLAGS=-lyaml -luuid
ALL:
	gcc server.c -o server ${CFLAGS} ${LDFLAGS}


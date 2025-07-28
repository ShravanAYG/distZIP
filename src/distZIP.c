#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <yaml.h>

#include "distZIP.h"

void print_table(table t)
{
	char uuid_str[37];
	uuid_unparse(t.uuid, uuid_str);
	printf("Recived:\n\tFrom: %s\n\tUUID: %s\n\tSize: %ld\n\tFile: %s\n\tStatus: %d\n", t.ip, uuid_str, t.size, t.filename, t.status);
}

int compress_file(const char *compressor, const char *file, table *t)
{
	char cmd[256];

	sprintf(cmd, "%s -n %s", compressor, file);

	FILE *p = popen(cmd, "r");
	if (!p) {
		perror("popen");
		return 1;
	}

	while (fgets(t->buf, sizeof(t->buf), p))
		printf("%s", t->buf);

	pclose(p);
	return 0;
}

int comperess_and_send(table *t)
{
	if (t->status != 1)
		return 0;
	print_table(*t);
	char fout[255];
	char Ruuid[37];
	uuid_unparse(t->uuid, Ruuid);
	sprintf(fout, "%s.gz", Ruuid);

	compress_file("gzip", Ruuid, t);

	FILE *dst = fopen(fout, "rb");
	struct stat st;
	stat(fout, &st);
	char *dstBuf = malloc(st.st_size);
	fread(dstBuf, 1, st.st_size, dst);
	fclose(dst);
	char header[512];
	size_t header_len = snprintf(header, sizeof(header), "%s.gz %s %ld\n", t->filename, Ruuid, st.st_size);
	size_t total_len = header_len + st.st_size;
	char *outBuf = malloc(total_len);
	memcpy(outBuf, header, header_len);
	memcpy(outBuf + header_len, dstBuf, st.st_size);
	int sockfd = connectToClient(t->ip, 9998);
	write(sockfd, outBuf, total_len);
	close(sockfd);

	free(dstBuf);
	free(outBuf);
	remove(fout);
	t->status = 2;
	return 1;
}

void create_table(table **t_ptr, size_t *t_cnt_ptr, size_t *tmax_ptr, const char *Ruuid, const char *filename, size_t rSize, const char *serverIP)
{
	if (*t_cnt_ptr >= *tmax_ptr) {
		*tmax_ptr *= 2;
		*t_ptr = realloc(*t_ptr, *tmax_ptr * sizeof(table));
	}
	table *t = *t_ptr;
	size_t i = *t_cnt_ptr;
	uuid_parse(Ruuid, t[i].uuid);
	strncpy(t[i].filename, filename, sizeof(t[i].filename) - 1);
	t[i].filename[sizeof(t[i].filename) - 1] = '\0';
	strncpy(t[i].ip, serverIP, sizeof(t[i].ip) - 1);
	t[i].ip[sizeof(t[i].ip) - 1] = '\0';
	t[i].size = rSize;
	t[i].status = 1;
	(*t_cnt_ptr)++;
}



char *configParse(const char *key_to_find, char *exc)
{
	FILE *fh = fopen("server.yml", "r");
	if (!fh) {
		char path[512];
		strncpy(path, exc, sizeof(path) - 1);
		path[sizeof(path) - 1] = '\0';
		char *p = strrchr(path, '/');
		if (p) {
			*(p + 1) = '\0';
			strncat(path, "server.yml", sizeof(path) - strlen(path) - 1);
			fh = fopen(path, "r");
		}

		if (!fh) {
			fprintf(stderr, "Configuration file server.yml not found\n");
			exit(1);
		}
	}
	yaml_parser_t parser;
	yaml_token_t token;
	int key_matched = 0;
	char *found_value = NULL;

	yaml_parser_initialize(&parser);
	yaml_parser_set_input_file(&parser, fh);

	do {
		yaml_parser_scan(&parser, &token);
		if (token.type == YAML_SCALAR_TOKEN) {
			char *value = (char *)token.data.scalar.value;
			if (key_matched) {
				found_value = strdup(value);
				key_matched = 0;
				break;
			}
			if (strcmp(value, key_to_find) == 0)
				key_matched = 1;
		}
		yaml_token_delete(&token);
	} while (token.type != YAML_STREAM_END_TOKEN);

	yaml_parser_delete(&parser);
	fclose(fh);

	return found_value;	// free this after use
}

char *getIPAddr(const char *interface)
{
	struct ifaddrs *ifAddrStruct = NULL;
	struct ifaddrs *ifa = NULL;
	void *tmpAddrPtr = NULL;
	char *s = calloc(255, sizeof(char));

	getifaddrs(&ifAddrStruct);

	for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr)
			continue;
		if (ifa->ifa_addr->sa_family == AF_INET) {
			tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
			char addressBuffer[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
			if (strcmp(interface, ifa->ifa_name) == 0)
				strcpy(s, addressBuffer);
		}
	}
	if (ifAddrStruct != NULL)
		freeifaddrs(ifAddrStruct);
	return s;

}

int connectToClient(const char *clientIP, int port)
{
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return -1;
	}
	struct sockaddr_in client_addr;
	memset(&client_addr, 0, sizeof(client_addr));
	client_addr.sin_family = AF_INET;
	client_addr.sin_port = htons(port);

	if (inet_pton(AF_INET, clientIP, &client_addr.sin_addr) <= 0) {
		perror("inet_pton");
		close(sockfd);
		return -1;
	}

	if (connect(sockfd, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
		perror("connect");
		close(sockfd);
		return -1;
	}
	return sockfd;		// connected socket
}

int listenOnIP(const char *ip, int port)
{
	int sockfd;
	struct sockaddr_in addr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return -1;
	}
	int opt = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
		perror("inet_pton");
		close(sockfd);
		return -1;
	}

	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(sockfd);
		return -1;
	}

	if (listen(sockfd, 5) < 0) {
		perror("listen");
		close(sockfd);
		return -1;
	}
	return sockfd;
}

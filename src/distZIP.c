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

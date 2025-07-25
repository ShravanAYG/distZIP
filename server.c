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

char *configParse(const char *key_to_find)
{
	FILE *fh = fopen("server.yml", "r");
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

int main(int argc, char **argv)
{
	char *interface = configParse("interface");
	char *clientIP = configParse("client");
	char *IPAddr = getIPAddr(interface);
	uuid_t binuuid;
	char uuid_str[37];
	if (argc < 2) {
		fprintf(stderr, "No file given\n");
		goto cleanup;
	}
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) {
		perror("Error in opening file");
		goto cleanup;
	}
	struct stat st;
	stat(argv[1], &st);
	char *buf = malloc(st.st_size + 128);

	uuid_generate(binuuid);
	uuid_unparse(binuuid, uuid_str);

	int sockfd = connectToClient(clientIP, 9999);
	int sockfdR = listenOnIP(IPAddr, 9998);
	if (sockfd < 0 || sockfdR < 0)
		goto cleanup;

	char msg[256];
	snprintf(msg, sizeof(msg), "%s %ld %s %s\n", argv[1], st.st_size, uuid_str, IPAddr);
	send(sockfd, msg, strlen(msg), 0);

	fread(buf, sizeof(char), st.st_size + 8, fp);
	send(sockfd, buf, st.st_size, 0);
	printf("sent %s\n", argv[1]);

	close(sockfd);

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	int client_fd = accept(sockfdR, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd < 0) {
		perror("accept");
		close(sockfdR);
	}

	ssize_t total = 0, n;
	char *newline = NULL;

	while ((n = read(client_fd, buf + total, 4096 - total - 1)) > 0) {
		total += n;
		buf[total] = '\0';
		newline = strchr(buf, '\n');
		if (newline)
			break;
	}
	char filename[256], Ruuid[64], recvsize[32];
	*newline = '\0';
	if (sscanf(buf, "%255s %63s %31s", filename, Ruuid, recvsize) != 3) {
		fprintf(stderr, "Invalid header format.\n");
		return 1;
	}
	size_t sizeR = atoi(recvsize);
	printf("Received: %s, Ratio: %.2f%%\n", filename, (double)(st.st_size - sizeR) * 100.0 / (double)st.st_size);
	FILE *out = fopen(filename, "wb");
	char *file_start = newline + 1;
	size_t already = total - (file_start - buf);
	fwrite(file_start, 1, already, out);
	size_t received = already;
	while (received < sizeR) {
		size_t to_read;
		if (sizeR - received > 4096)
			to_read = 4096;
		else
			to_read = sizeR - received;
		n = read(client_fd, buf, to_read);
		if (n <= 0) {
			perror("read");
			break;
		}
		fwrite(buf, 1, n, out);
		received += n;
	}
	fclose(out);

	close(client_fd);
	close(sockfdR);

	fclose(fp);
 cleanup:

	free(buf);
	free(IPAddr);
	free(interface);
	free(clientIP);
	return 0;
}

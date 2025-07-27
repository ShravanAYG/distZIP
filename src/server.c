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

int main(int argc, char **argv)
{
	char *interface = configParse("interface", argv[0]);
	char *clientIP = configParse("client", argv[0]);
	char *IPAddr = getIPAddr(interface);
	uuid_t binuuid;
	char *buf = NULL;
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
	buf = malloc(st.st_size + 128);

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

	if (buf != NULL)
		free(buf);
	free(IPAddr);
	free(interface);
	free(clientIP);
	return 0;
}

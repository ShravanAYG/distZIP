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

int main()
{
	char buf[4096];
	char *interface = configParse("interface");
	char *IPAddr = getIPAddr(interface);
	int sockfdR = listenOnIP(IPAddr, 9999);
	if (sockfdR < 0)
		goto cleanup;

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	int client_fd = accept(sockfdR, (struct sockaddr *)&client_addr, &client_len);
	if (client_fd < 0) {
		perror("accept");
		close(sockfdR);
		goto cleanup;
	}

	ssize_t total = 0, n;
	while ((n = read(client_fd, buf + total, sizeof(buf) - total - 1)) > 0) {
		total += n;
		if ((size_t)total >= sizeof(buf) - 1)
			break;
		buf[total] = '\0';
	}
	char filename[256], Ruuid[64], recvsize[32], serverIP[64];
	if (sscanf(buf, "%255s %31s %63s %64s", filename, recvsize, Ruuid, serverIP) != 4) {
		fprintf(stderr, "Invalid header format.\n");
		return 1;
	}
	size_t rSize = atoi(recvsize);
	printf("Recived:\n\tFrom: %s\n\tUUID: %s\n\tSize: %ld\n\tFile: %s\n", serverIP, Ruuid, rSize, filename);
	char *newline = strchr(buf, '\n');
	FILE *out = fopen(Ruuid, "wb");
	char *file_start = newline + 1;
	size_t already = total - (file_start - buf);
	fwrite(file_start, 1, already, out);
	size_t received = already;
	while (received < rSize) {
		size_t to_read;
		if (rSize - received > 4096)
			to_read = 4096;
		else
			to_read = rSize - received;
		n = read(client_fd, buf, to_read);
		if (n <= 0) {
			perror("read");
			break;
		}
		fwrite(buf, 1, n, out);
		received += n;
	}
	fclose(out);

	char cmd[256];

	sprintf(cmd, "gzip %s", Ruuid);

	FILE *p = popen(cmd, "r");
	if (!p) {
		perror("popen");
		return 1;
	}

	char pout[256];
	while (fgets(buf, sizeof(pout), p))
		printf("%s", pout);

	pclose(p);

	close(client_fd);
	close(sockfdR);

	char fout[255];
	sprintf(fout, "%s.gz", Ruuid);

	FILE *dst = fopen(fout, "rb");
	struct stat st;
	stat(fout, &st);
	char *dstBuf = malloc(st.st_size);
	fread(dstBuf, 1, st.st_size, dst);
	fclose(dst);
	char header[512];
	size_t header_len = sprintf(header, "%s.gz %s %ld\n", filename, Ruuid, st.st_size);
	size_t total_len = header_len + st.st_size;
	char *outBuf = malloc(total_len);
	memcpy(outBuf, header, header_len);
	memcpy(outBuf + header_len, dstBuf, st.st_size);
	int sockfd = connectToClient(serverIP, 9998);
	write(sockfd, outBuf, total_len);
	close(sockfd);

	free(dstBuf);
	free(outBuf);
	remove(fout);

 cleanup:
	free(IPAddr);
	free(interface);

	return 0;
}

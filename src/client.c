#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <yaml.h>

#include "distZIP.h"

#define BUF_SIZE 4096

static volatile sig_atomic_t keep_running = 1;

static void sig_handler(int _)
{
	(void)_;
	keep_running = 0;
}

int main(int argc, char *argv[])
{
	if (argc > 1)
		return 1;
	signal(SIGINT, sig_handler);
	size_t t_cnt = 0;

	char *interface = configParse("interface", argv[0]);
	char *IPAddr = getIPAddr(interface);
	int sockfdR = listenOnIP(IPAddr, 9999);

	if (sockfdR < 0) {
		perror("listenOnIP");
		goto cleanup;
	}

	while (keep_running) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = accept(sockfdR, (struct sockaddr *)&client_addr, &client_len);
		if (client_fd < 0) {
			perror("accept");
			continue;
		}

		char buf[BUF_SIZE];
		ssize_t total = 0, n;
		while (((n = read(client_fd, buf + total, sizeof(buf) - total - 1)) > 0)) {
			total += n;
			if ((size_t)total >= sizeof(buf) - 1)
				break;
			buf[total] = '\0';
		}

		char filename[256], Ruuid[64], recvsize[32], serverIP[64];
		if (sscanf(buf, "%255s %31s %63s %64s", filename, recvsize, Ruuid, serverIP) != 4) {
			fprintf(stderr, "Invalid header format.\n");
			close(client_fd);
			continue;
		}

		size_t rSize = atoi(recvsize);
		create_table(Ruuid, filename, rSize, serverIP);

		char *newline = strchr(buf, '\n');
		FILE *out = fopen(Ruuid, "wb");
		if (!out) {
			perror("fopen");
			close(client_fd);
			continue;
		}

		char *file_start = newline + 1;
		size_t already = total - (file_start - buf);
		fwrite(file_start, 1, already, out);

		size_t received = already;
		while (received < rSize) {
			size_t to_read = (rSize - received > BUF_SIZE) ? BUF_SIZE : rSize - received;
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

		t_cnt++;
		comperess_and_send();
	}

	close(sockfdR);

 cleanup:
	free(IPAddr);
	free(interface);
	printf("\nProcessed %ld files\n", t_cnt);
	return 0;
}

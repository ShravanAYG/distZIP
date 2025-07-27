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

typedef struct {
	uuid_t uuid;
	char filename[1024];
	size_t size;
	int status;
	char ip[255], buf[512];
} table;

table *tb;
size_t t_cnt = 0, tmax = 32;

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

static volatile sig_atomic_t keep_running = 1;

static void sig_handler(int _)
{
	(void)_;
	keep_running = 0;
}

int main(int argc, char *argv[])
{
	if(argc > 1)
		return 1;
	signal(SIGINT, sig_handler);

	char *interface = configParse("interface", argv[0]);
	char *IPAddr = getIPAddr(interface);
	int sockfdR = listenOnIP(IPAddr, 9999);

	if (sockfdR < 0) {
		perror("listenOnIP");
		goto cleanup;
	}

	size_t t_cnt = 0, tmax = 32;
	table *tb = calloc(tmax, sizeof(table));

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
		while ((n = read(client_fd, buf + total, sizeof(buf) - total - 1)) > 0) {
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
		create_table(&tb, &t_cnt, &tmax, Ruuid, filename, rSize, serverIP);

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

		comperess_and_send(&tb[t_cnt - 1]);
	}

	close(sockfdR);

 cleanup:
	free(tb);
	free(IPAddr);
	free(interface);
	printf("\nProcessed %ld files\n", t_cnt);
	return 0;
}

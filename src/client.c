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
	printf("%s\n", buf);

	close(client_fd);
	close(sockfdR);

 cleanup:
	free(IPAddr);
	free(interface);
	return 0;
}

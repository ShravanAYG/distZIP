#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "distZIP.h"
#include "protocol.h"

#define CONFIG_PATH "client.yml"

static volatile sig_atomic_t keep_running = 1;
static void sig_handler(int sig) {
  (void)sig;
  keep_running = 0;
}

typedef struct {
  job_t job;
  char server_ip[MAX_IP_LEN];
  int server_result_port;
} work_item_t;

static void *worker_thread(void *arg) {
  work_item_t *w = (work_item_t *)arg;
  char uuid_str[MAX_UUID_STR_LEN];
  uuid_unparse(w->job.uuid, uuid_str);

  printf("[client] Compressing chunk %d/%d of %s (%zu B)\n",
         w->job.chunk_index + 1, w->job.total_chunks, w->job.filename,
         w->job.chunk_size);

  char *compressed = NULL;
  size_t csz = 0;
  if (gzip_compress(w->job.chunk_data, w->job.chunk_size, &compressed, &csz) <
      0) {
    fprintf(stderr, "[client] Compression failed for %s chunk %d\n", uuid_str,
            w->job.chunk_index);
    free(w->job.chunk_data);
    free(w);
    return NULL;
  }

  printf("[client] Compressed: %zu → %zu B (%.1f%%)\n", w->job.chunk_size, csz,
         (double)csz * 100.0 / w->job.chunk_size);

  int fd = connectToHost(w->server_ip, w->server_result_port);
  if (fd < 0) {
    fprintf(stderr, "[client] Cannot connect to server %s:%d for result\n",
            w->server_ip, w->server_result_port);
    free(compressed);
    free(w->job.chunk_data);
    free(w);
    return NULL;
  }

  char header[MAX_HEADER_LEN];
  int hlen =
      snprintf(header, sizeof(header), TAG_RESULT " %s %zu\n", uuid_str, csz);

  if (sendall(fd, header, hlen) < 0 || sendall(fd, compressed, csz) < 0) {
    fprintf(stderr, "[client] Failed sending result %s\n", uuid_str);
  } else {
    printf("[client] Sent result chunk %d/%d → %s\n", w->job.chunk_index + 1,
           w->job.total_chunks, w->server_ip);
  }

  close(fd);
  free(compressed);
  free(w->job.chunk_data);
  free(w);
  return NULL;
}

static int try_register(const client_config_t *cfg) {
  int fd = connectToHost(cfg->server_address, cfg->server_port);
  if (fd < 0)
    return -1;

  char msg[MAX_HEADER_LEN];
  int len = snprintf(msg, sizeof(msg), TAG_PONG " %s\n", cfg->id);
  sendall(fd, msg, len);
  close(fd);
  printf("[client] Registered with server %s:%d as '%s'\n", cfg->server_address,
         cfg->server_port, cfg->id);
  return 0;
}

static void *register_thread(void *arg) {
  const client_config_t *cfg = (const client_config_t *)arg;
  while (keep_running) {
    if (try_register(cfg) == 0)
      return NULL;
    sleep(2);
  }
  return NULL;
}

static void handle_connection(int fd, const client_config_t *cfg,
                              struct sockaddr_in *peer) {
  char peer_ip[MAX_IP_LEN];
  inet_ntop(AF_INET, &peer->sin_addr, peer_ip, sizeof(peer_ip));

  char header[MAX_HEADER_LEN];
  int hlen = recv_header_line(fd, header, sizeof(header));
  if (hlen <= 0)
    return;

  msg_type_t mtype = parse_msg_type(header);

  if (mtype == MSG_PING) {
    char resp[MAX_HEADER_LEN];
    int rlen = snprintf(resp, sizeof(resp), TAG_PONG " %s\n", cfg->id);
    sendall(fd, resp, rlen);
    printf("[client] PONG → %s\n", peer_ip);
    return;
  }

  if (mtype == MSG_JOB) {
    /* Parse: DISTZIP-JOB <uuid> <filename> <chunk_idx> <total> <size> */
    char uuid_str[MAX_UUID_STR_LEN];
    char filename[MAX_FILENAME_LEN];
    int chunk_idx, total_chunks;
    size_t chunk_size;

    if (sscanf(header, TAG_JOB " %36s %1023s %d %d %zu", uuid_str, filename,
               &chunk_idx, &total_chunks, &chunk_size) != 5) {
      fprintf(stderr, "[client] Bad job header: %s\n", header);
      return;
    }

    char *data = malloc(chunk_size);
    if (!data || recvall(fd, data, chunk_size) < 0) {
      fprintf(stderr, "[client] Failed receiving chunk data\n");
      free(data);
      return;
    }

    printf("[client] Received job %s (chunk %d/%d, %zu B) from %s\n", uuid_str,
           chunk_idx + 1, total_chunks, chunk_size, peer_ip);

    work_item_t *w = malloc(sizeof(work_item_t));
    uuid_parse(uuid_str, w->job.uuid);
    strncpy(w->job.filename, filename, MAX_FILENAME_LEN - 1);
    w->job.chunk_index = chunk_idx;
    w->job.total_chunks = total_chunks;
    w->job.chunk_size = chunk_size;
    w->job.chunk_data = data;
    strncpy(w->server_ip, peer_ip, MAX_IP_LEN - 1);
    w->server_result_port = cfg->server_port + 1; /* results on port+1 */

    pthread_t tid;
    pthread_create(&tid, NULL, worker_thread, w);
    pthread_detach(tid);
  }
}

int main(int argc, char **argv) {
  (void)argc;
  signal(SIGINT, sig_handler);
  signal(SIGPIPE, SIG_IGN);

  char *cfgpath = CONFIG_PATH;
  int opt;
  while ((opt = getopt(argc, argv, "c:h")) != -1) {
    switch (opt) {
    case 'c':
      cfgpath = optarg;
      break;
    default:
      fprintf(stderr, "Usage: %s [-c config]\n", argv[0]);
      return 1;
    }
  }
  static client_config_t cfg;
  parse_client_config(cfgpath, &cfg);

  char *rip = NULL;
  if (cfg.listen_address[0] == '\0' ||
      strcmp(cfg.listen_address, "0.0.0.0") == 0) {
    if (cfg.interface[0]) {
      rip = getIPAddr(cfg.interface);
      strncpy(cfg.listen_address, rip, MAX_IP_LEN - 1);
    }
  }

  /* Create temp directory */
  mkdir(cfg.temp_dir, 0755);

  printf("[client] distZIP Client '%s'\n", cfg.id);
  printf("[client] Listen: %s:%d  Server: %s:%d\n", cfg.listen_address,
         cfg.port, cfg.server_address, cfg.server_port);

  pthread_t reg_tid;
  pthread_create(&reg_tid, NULL, register_thread, &cfg);
  pthread_detach(reg_tid);

  int sockfd = listenOnIP(cfg.listen_address, cfg.port);
  if (sockfd < 0) {
    fprintf(stderr, "[client] Cannot listen on %s:%d\n", cfg.listen_address,
            cfg.port);
    free(rip);
    return 1;
  }

  printf("[client] Listening for jobs...\n");
  size_t jobs_done = 0;

  while (keep_running) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(sockfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept");
      continue;
    }

    handle_connection(fd, &cfg, &peer);
    close(fd);
    jobs_done++;
  }

  close(sockfd);
  free(rip);
  printf("\n[client] Processed %zu jobs. Exiting.\n", jobs_done);
  return 0;
}

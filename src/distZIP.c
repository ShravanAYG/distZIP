
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <yaml.h>
#include <zlib.h>

#include "distZIP.h"

static char *yaml_lookup(const char *path, const char *dotted_key) {
  FILE *fh = fopen(path, "r");
  if (!fh) {
    fprintf(stderr, "Cannot open config: %s\n", path);
    return NULL;
  }

  yaml_parser_t parser;
  yaml_event_t event;
  yaml_parser_initialize(&parser);
  yaml_parser_set_input_file(&parser, fh);

  char section[128] = {0}, key[128] = {0};
  const char *dot = strchr(dotted_key, '.');
  if (dot) {
    strncpy(section, dotted_key, dot - dotted_key);
    strncpy(key, dot + 1, sizeof(key) - 1);
  } else {
    strncpy(key, dotted_key, sizeof(key) - 1);
  }

  int depth = 0;
  int in_section = (section[0] == '\0');
  int key_matched = 0;
  char *result = NULL;

  while (1) {
    if (!yaml_parser_parse(&parser, &event))
      break;
    if (event.type == YAML_STREAM_END_EVENT) {
      yaml_event_delete(&event);
      break;
    }

    if (event.type == YAML_MAPPING_START_EVENT) {
      depth++;
    } else if (event.type == YAML_MAPPING_END_EVENT) {
      if (depth == 1 && in_section && section[0] != '\0')
        in_section = 0;
      depth--;
    } else if (event.type == YAML_SCALAR_EVENT) {
      const char *val = (const char *)event.data.scalar.value;
      if (key_matched) {
        result = strdup(val);
        yaml_event_delete(&event);
        break;
      }
      if (section[0] != '\0' && depth == 1 && strcmp(val, section) == 0) {
        in_section = 1;
      }
      if (in_section && strcmp(val, key) == 0) {
        key_matched = 1;
      }
    }
    yaml_event_delete(&event);
  }

  yaml_parser_delete(&parser);
  fclose(fh);
  return result;
}

static int yaml_lookup_int(const char *path, const char *dotted_key,
                           int fallback) {
  char *val = yaml_lookup(path, dotted_key);
  if (!val)
    return fallback;
  int n = atoi(val);
  free(val);
  return n;
}

int parse_server_config(const char *path, server_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));

  char *v;
  if ((v = yaml_lookup(path, "network.interface"))) {
    strncpy(cfg->interface, v, sizeof(cfg->interface) - 1);
    free(v);
  }
  if ((v = yaml_lookup(path, "network.bind_address"))) {
    strncpy(cfg->bind_address, v, sizeof(cfg->bind_address) - 1);
    free(v);
  }
  if ((v = yaml_lookup(path, "network.subnet"))) {
    strncpy(cfg->subnet, v, sizeof(cfg->subnet) - 1);
    free(v);
  }
  cfg->port = yaml_lookup_int(path, "network.port", DEFAULT_SERVER_PORT);

  if ((v = yaml_lookup(path, "compression.format"))) {
    strncpy(cfg->format, v, sizeof(cfg->format) - 1);
    free(v);
  } else {
    strcpy(cfg->format, "gzip");
  }
  cfg->chunk_size_mb = yaml_lookup_int(path, "compression.chunk_size_mb", 64);

  cfg->max_clients = yaml_lookup_int(path, "scheduler.max_clients", 8);
  cfg->retry_failed_jobs =
      yaml_lookup_int(path, "scheduler.retry_failed_jobs", 1);

  return 0;
}

int parse_client_config(const char *path, client_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));

  char *v;
  if ((v = yaml_lookup(path, "client.id"))) {
    strncpy(cfg->id, v, sizeof(cfg->id) - 1);
    free(v);
  } else {
    strcpy(cfg->id, "client-unknown");
  }
  if ((v = yaml_lookup(path, "client.listen_address"))) {
    strncpy(cfg->listen_address, v, sizeof(cfg->listen_address) - 1);
    free(v);
  } else {
    strcpy(cfg->listen_address, "0.0.0.0");
  }
  if ((v = yaml_lookup(path, "client.interface"))) {
    strncpy(cfg->interface, v, sizeof(cfg->interface) - 1);
    free(v);
  }
  cfg->port = yaml_lookup_int(path, "client.port", DEFAULT_CLIENT_PORT);

  if ((v = yaml_lookup(path, "server.address"))) {
    strncpy(cfg->server_address, v, sizeof(cfg->server_address) - 1);
    free(v);
  }
  cfg->server_port = yaml_lookup_int(path, "server.port", DEFAULT_SERVER_PORT);

  if ((v = yaml_lookup(path, "compression.temp_dir"))) {
    strncpy(cfg->temp_dir, v, sizeof(cfg->temp_dir) - 1);
    free(v);
  } else {
    strcpy(cfg->temp_dir, "/tmp/distzip");
  }

  return 0;
}

char *getIPAddr(const char *interface) {
  struct ifaddrs *ifAddrStruct = NULL;
  struct ifaddrs *ifa = NULL;
  char *s = calloc(MAX_IP_LEN, sizeof(char));

  getifaddrs(&ifAddrStruct);

  for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr)
      continue;
    if (ifa->ifa_addr->sa_family == AF_INET) {
      void *addr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
      char buf[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, addr, buf, INET_ADDRSTRLEN);
      if (strcmp(interface, ifa->ifa_name) == 0)
        strncpy(s, buf, MAX_IP_LEN - 1);
    }
  }
  if (ifAddrStruct)
    freeifaddrs(ifAddrStruct);
  return s;
}

int connectToHost(const char *ip, int port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    perror("inet_pton");
    close(sockfd);
    return -1;
  }
  if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(sockfd);
    return -1;
  }
  return sockfd;
}

int listenOnIP(const char *ip, int port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  int opt = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
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
  if (listen(sockfd, 16) < 0) {
    perror("listen");
    close(sockfd);
    return -1;
  }
  return sockfd;
}

int sendall(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, p + sent, len - sent);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      return -1;
    }
    sent += n;
  }
  return 0;
}

int recvall(int fd, void *buf, size_t len) {
  char *p = (char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, p + got, len - got);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      return -1;
    }
    got += n;
  }
  return 0;
}

int recv_header_line(int fd, char *buf, size_t bufsize) {
  size_t pos = 0;
  while (pos < bufsize - 1) {
    char c;
    ssize_t n = read(fd, &c, 1);
    if (n <= 0)
      return -1;
    if (c == '\n') {
      buf[pos] = '\0';
      return (int)pos;
    }
    buf[pos++] = c;
  }
  buf[pos] = '\0';
  return (int)pos;
}

int is_ip_in_subnet(const char *ip, const char *cidr) {
  if (!cidr || cidr[0] == '\0')
    return 1; /* no subnet = allow all */

  char cidr_copy[64];
  strncpy(cidr_copy, cidr, sizeof(cidr_copy) - 1);
  cidr_copy[sizeof(cidr_copy) - 1] = '\0';

  char *slash = strchr(cidr_copy, '/');
  if (!slash)
    return 1;
  *slash = '\0';
  int prefix_len = atoi(slash + 1);

  struct in_addr net_addr, test_addr;
  if (inet_pton(AF_INET, cidr_copy, &net_addr) != 1)
    return 0;
  if (inet_pton(AF_INET, ip, &test_addr) != 1)
    return 0;

  uint32_t mask =
      (prefix_len == 0) ? 0 : htonl(~((1U << (32 - prefix_len)) - 1));
  return (net_addr.s_addr & mask) == (test_addr.s_addr & mask);
}

int split_file_into_jobs(const char *filepath, size_t chunk_size_bytes,
                         job_t **jobs_out, int *count_out) {
  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    perror("fopen");
    return -1;
  }

  struct stat st;
  if (stat(filepath, &st) != 0) {
    perror("stat");
    fclose(fp);
    return -1;
  }
  size_t file_size = st.st_size;

  if (chunk_size_bytes == 0)
    chunk_size_bytes = file_size; /* single chunk */

  int total_chunks =
      (int)((file_size + chunk_size_bytes - 1) / chunk_size_bytes);
  if (total_chunks == 0)
    total_chunks = 1;

  job_t *jobs = calloc(total_chunks, sizeof(job_t));
  if (!jobs) {
    fclose(fp);
    return -1;
  }

  for (int i = 0; i < total_chunks; i++) {
    uuid_generate(jobs[i].uuid);
    strncpy(jobs[i].filename, filepath, MAX_FILENAME_LEN - 1);
    jobs[i].chunk_index = i;
    jobs[i].total_chunks = total_chunks;
    jobs[i].state = JOB_PENDING;
    jobs[i].retries = 0;
    jobs[i].result_data = NULL;

    size_t offset = (size_t)i * chunk_size_bytes;
    size_t this_chunk = chunk_size_bytes;
    if (offset + this_chunk > file_size)
      this_chunk = file_size - offset;

    jobs[i].chunk_size = this_chunk;
    jobs[i].chunk_data = malloc(this_chunk);
    if (!jobs[i].chunk_data) {
      /* Cleanup on failure */
      for (int j = 0; j < i; j++)
        free(jobs[j].chunk_data);
      free(jobs);
      fclose(fp);
      return -1;
    }

    fseek(fp, offset, SEEK_SET);
    if (fread(jobs[i].chunk_data, 1, this_chunk, fp) != this_chunk) {
      fprintf(stderr, "Short read on chunk %d\n", i);
      for (int j = 0; j <= i; j++)
        free(jobs[j].chunk_data);
      free(jobs);
      fclose(fp);
      return -1;
    }
  }

  fclose(fp);
  *jobs_out = jobs;
  *count_out = total_chunks;
  return 0;
}

void free_jobs(job_t *jobs, int count) {
  if (!jobs)
    return;
  for (int i = 0; i < count; i++) {
    free(jobs[i].chunk_data);
    free(jobs[i].result_data);
  }
  free(jobs);
}

int gzip_compress(const char *in, size_t in_len, char **out, size_t *out_len) {
  uLong bound = compressBound(in_len) + 64;
  *out = malloc(bound);
  if (!*out)
    return -1;

  z_stream strm;
  memset(&strm, 0, sizeof(strm));

  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    free(*out);
    *out = NULL;
    return -1;
  }

  strm.next_in = (Bytef *)in;
  strm.avail_in = in_len;
  strm.next_out = (Bytef *)*out;
  strm.avail_out = bound;

  int ret = deflate(&strm, Z_FINISH);
  if (ret != Z_STREAM_END) {
    deflateEnd(&strm);
    free(*out);
    *out = NULL;
    return -1;
  }

  *out_len = strm.total_out;
  deflateEnd(&strm);
  return 0;
}

int gzip_decompress(const char *in, size_t in_len, char **out,
                    size_t *out_len) {
  size_t alloc = in_len * 4;
  if (alloc < 4096)
    alloc = 4096;
  *out = malloc(alloc);
  if (!*out)
    return -1;

  z_stream strm;
  memset(&strm, 0, sizeof(strm));

  if (inflateInit2(&strm, 15 + 16) != Z_OK) {
    free(*out);
    *out = NULL;
    return -1;
  }

  strm.next_in = (Bytef *)in;
  strm.avail_in = in_len;
  strm.next_out = (Bytef *)*out;
  strm.avail_out = alloc;

  int ret;
  while ((ret = inflate(&strm, Z_NO_FLUSH)) != Z_STREAM_END) {
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      inflateEnd(&strm);
      free(*out);
      *out = NULL;
      return -1;
    }
    if (strm.avail_out == 0) {
      size_t new_alloc = alloc * 2;
      char *tmp = realloc(*out, new_alloc);
      if (!tmp) {
        inflateEnd(&strm);
        free(*out);
        *out = NULL;
        return -1;
      }
      *out = tmp;
      strm.next_out = (Bytef *)(*out + alloc);
      strm.avail_out = new_alloc - alloc;
      alloc = new_alloc;
    }
  }

  *out_len = strm.total_out;
  inflateEnd(&strm);
  return 0;
}

typedef struct jq_node {
  job_t job;
  struct jq_node *next;
} jq_node;

static jq_node *jq_head = NULL;
static jq_node *jq_tail = NULL;
static pthread_mutex_t jq_lock = PTHREAD_MUTEX_INITIALIZER;

void jq_init(void) {
  pthread_mutex_lock(&jq_lock);
  jq_head = jq_tail = NULL;
  pthread_mutex_unlock(&jq_lock);
}

void jq_destroy(void) {
  pthread_mutex_lock(&jq_lock);
  jq_node *cur = jq_head;
  while (cur) {
    jq_node *tmp = cur;
    cur = cur->next;
    free(tmp->job.chunk_data);
    free(tmp->job.result_data);
    free(tmp);
  }
  jq_head = jq_tail = NULL;
  pthread_mutex_unlock(&jq_lock);
}

void jq_push(job_t *job) {
  jq_node *node = malloc(sizeof(jq_node));
  memcpy(&node->job, job, sizeof(job_t));
  node->next = NULL;

  pthread_mutex_lock(&jq_lock);
  if (!jq_tail) {
    jq_head = jq_tail = node;
  } else {
    jq_tail->next = node;
    jq_tail = node;
  }
  pthread_mutex_unlock(&jq_lock);
}

job_t *jq_pop(void) {
  pthread_mutex_lock(&jq_lock);
  if (!jq_head) {
    pthread_mutex_unlock(&jq_lock);
    return NULL;
  }
  jq_node *node = jq_head;
  jq_head = jq_head->next;
  if (!jq_head)
    jq_tail = NULL;
  pthread_mutex_unlock(&jq_lock);

  job_t *job = malloc(sizeof(job_t));
  memcpy(job, &node->job, sizeof(job_t));
  free(node);
  return job;
}

int jq_count(void) {
  pthread_mutex_lock(&jq_lock);
  int count = 0;
  jq_node *cur = jq_head;
  while (cur) {
    count++;
    cur = cur->next;
  }
  pthread_mutex_unlock(&jq_lock);
  return count;
}

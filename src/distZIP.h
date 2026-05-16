#ifndef DISTZIP_H
#define DISTZIP_H

#include "protocol.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <uuid/uuid.h>

typedef enum {
  JOB_PENDING = 0,
  JOB_ASSIGNED,
  JOB_COMPLETE,
  JOB_FAILED
} job_state_t;

typedef struct {
  uuid_t uuid;
  char filename[MAX_FILENAME_LEN];
  int chunk_index;
  int total_chunks;
  size_t chunk_size;      /* bytes in this chunk             */
  size_t compressed_size; /* filled after compression        */
  job_state_t state;
  char assigned_client[MAX_IP_LEN];
  int retries;
  time_t assigned_time;
  char *chunk_data;  /* raw chunk bytes (server-side)    */
  char *result_data; /* compressed bytes (server-side)   */
} job_t;

typedef enum { CLIENT_IDLE = 0, CLIENT_BUSY, CLIENT_DEAD } client_state_t;

typedef struct {
  char id[MAX_CLIENT_ID_LEN];
  char ip[MAX_IP_LEN];
  int port;
  client_state_t state;
  time_t last_seen;
} client_info_t;

typedef struct {
  char interface[64];
  char bind_address[MAX_IP_LEN];
  char subnet[32]; /* CIDR notation */
  int port;
  char format[16]; /* "gzip" */
  size_t chunk_size_mb;
  int max_clients;
  int retry_failed_jobs;
} server_config_t;

typedef struct {
  char id[MAX_CLIENT_ID_LEN];
  char listen_address[MAX_IP_LEN];
  char interface[64];
  int port;
  char server_address[MAX_IP_LEN];
  int server_port;
  char temp_dir[512];
} client_config_t;

int parse_server_config(const char *path, server_config_t *cfg);
int parse_client_config(const char *path, client_config_t *cfg);

char *getIPAddr(const char *interface);
int connectToHost(const char *ip, int port);
int listenOnIP(const char *ip, int port);
int sendall(int fd, const void *buf, size_t len);
int recvall(int fd, void *buf, size_t len);
int recv_header_line(int fd, char *buf, size_t bufsize);

int is_ip_in_subnet(const char *ip, const char *cidr);

int split_file_into_jobs(const char *filepath, size_t chunk_size_bytes,
                         job_t **jobs_out, int *count_out);
void free_jobs(job_t *jobs, int count);

int gzip_compress(const char *in, size_t in_len, char **out, size_t *out_len);
int gzip_decompress(const char *in, size_t in_len, char **out, size_t *out_len);
void jq_init(void);
void jq_destroy(void);
void jq_push(job_t *job);
job_t *jq_pop(void); /* returns NULL if empty */
int jq_count(void);

#endif /* DISTZIP_H */

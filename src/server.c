/*
 * distZIP Server — Job Coordinator
 *
 * Splits files into chunks, discovers clients, dispatches compression
 * jobs, collects results, reassembles compressed output.
 */

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

#define JOB_TIMEOUT_SECS  60
#define MAX_RETRIES       3
#define CONFIG_PATH       "server.yml"

static volatile sig_atomic_t keep_running = 1;
static void sig_handler(int sig) { (void)sig; keep_running = 0; }

/* ── Client Registry ── */
static client_info_t  clients[64];
static int            client_count = 0;
static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

static int register_client(const char *ip, int port, const char *id)
{
    pthread_mutex_lock(&client_lock);
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].ip, ip) == 0) {
            clients[i].state = CLIENT_IDLE;
            clients[i].last_seen = time(NULL);
            strncpy(clients[i].id, id, MAX_CLIENT_ID_LEN - 1);
            pthread_mutex_unlock(&client_lock);
            return i;
        }
    }
    if (client_count >= 64) { pthread_mutex_unlock(&client_lock); return -1; }
    int idx = client_count++;
    strncpy(clients[idx].ip, ip, MAX_IP_LEN - 1);
    strncpy(clients[idx].id, id, MAX_CLIENT_ID_LEN - 1);
    clients[idx].port = port;
    clients[idx].state = CLIENT_IDLE;
    clients[idx].last_seen = time(NULL);
    pthread_mutex_unlock(&client_lock);
    printf("[server] Registered client: %s (%s)\n", id, ip);
    return idx;
}

static int find_idle_client(int start)
{
    pthread_mutex_lock(&client_lock);
    if (client_count == 0) { pthread_mutex_unlock(&client_lock); return -1; }
    for (int i = 0; i < client_count; i++) {
        int idx = (start + i) % client_count;
        if (clients[idx].state == CLIENT_IDLE) {
            pthread_mutex_unlock(&client_lock);
            return idx;
        }
    }
    pthread_mutex_unlock(&client_lock);
    return -1;
}

static void set_client_state(int idx, client_state_t state)
{
    pthread_mutex_lock(&client_lock);
    if (idx >= 0 && idx < client_count) clients[idx].state = state;
    pthread_mutex_unlock(&client_lock);
}

/* ── Discovery Thread ── */
typedef struct { server_config_t *cfg; int listen_fd; } discovery_ctx_t;

static void *discovery_thread(void *arg)
{
    discovery_ctx_t *ctx = (discovery_ctx_t *)arg;
    while (keep_running) {
        struct sockaddr_in peer; socklen_t plen = sizeof(peer);
        int fd = accept(ctx->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) { if (errno == EINTR) continue; break; }

        char peer_ip[MAX_IP_LEN];
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));

        if (ctx->cfg->subnet[0] && !is_ip_in_subnet(peer_ip, ctx->cfg->subnet)) {
            fprintf(stderr, "[server] Rejected %s (not in %s)\n", peer_ip, ctx->cfg->subnet);
            close(fd); continue;
        }

        char header[MAX_HEADER_LEN];
        if (recv_header_line(fd, header, sizeof(header)) <= 0) { close(fd); continue; }
        if (parse_msg_type(header) == MSG_PONG) {
            char cid[MAX_CLIENT_ID_LEN] = {0};
            sscanf(header, TAG_PONG " %127s", cid);
            register_client(peer_ip, DEFAULT_CLIENT_PORT, cid);
        }
        close(fd);
    }
    return NULL;
}

/* ── Dispatch a job to client ── */
static int dispatch_job(job_t *job, int cidx)
{
    pthread_mutex_lock(&client_lock);
    char ip[MAX_IP_LEN]; int port;
    strncpy(ip, clients[cidx].ip, MAX_IP_LEN);
    port = clients[cidx].port;
    pthread_mutex_unlock(&client_lock);

    int fd = connectToHost(ip, port);
    if (fd < 0) return -1;

    char uuid_str[MAX_UUID_STR_LEN];
    uuid_unparse(job->uuid, uuid_str);
    char header[MAX_HEADER_LEN];
    int hlen = snprintf(header, sizeof(header), TAG_JOB " %s %s %d %d %zu\n",
        uuid_str, job->filename, job->chunk_index, job->total_chunks, job->chunk_size);

    if (sendall(fd, header, hlen) < 0 || sendall(fd, job->chunk_data, job->chunk_size) < 0) {
        close(fd); return -1;
    }
    close(fd);

    job->state = JOB_ASSIGNED;
    job->assigned_time = time(NULL);
    strncpy(job->assigned_client, ip, MAX_IP_LEN - 1);
    printf("[server] Dispatched chunk %d/%d (%zu B) → %s\n",
           job->chunk_index + 1, job->total_chunks, job->chunk_size, ip);
    return 0;
}

/* ── Result Collector Thread ── */
typedef struct {
    int listen_fd; job_t *jobs; int job_count;
    int *completed; pthread_mutex_t *jlock; server_config_t *cfg;
} collector_ctx_t;

static void *collector_thread(void *arg)
{
    collector_ctx_t *ctx = (collector_ctx_t *)arg;
    while (keep_running && *ctx->completed < ctx->job_count) {
        struct sockaddr_in peer; socklen_t plen = sizeof(peer);
        int fd = accept(ctx->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) { if (errno == EINTR) continue; break; }

        char peer_ip[MAX_IP_LEN];
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));

        char header[MAX_HEADER_LEN];
        if (recv_header_line(fd, header, sizeof(header)) <= 0) { close(fd); continue; }

        if (parse_msg_type(header) == MSG_PONG) {
            char cid[MAX_CLIENT_ID_LEN] = {0};
            sscanf(header, TAG_PONG " %127s", cid);
            register_client(peer_ip, DEFAULT_CLIENT_PORT, cid);
            close(fd); continue;
        }
        if (parse_msg_type(header) != MSG_RESULT) { close(fd); continue; }

        char uuid_str[MAX_UUID_STR_LEN]; size_t csz;
        if (sscanf(header, TAG_RESULT " %36s %zu", uuid_str, &csz) != 2) { close(fd); continue; }

        char *data = malloc(csz);
        if (!data || recvall(fd, data, csz) < 0) { free(data); close(fd); continue; }
        close(fd);

        uuid_t ruuid; uuid_parse(uuid_str, ruuid);
        pthread_mutex_lock(ctx->jlock);
        for (int i = 0; i < ctx->job_count; i++) {
            if (uuid_compare(ctx->jobs[i].uuid, ruuid) == 0) {
                ctx->jobs[i].result_data = data;
                ctx->jobs[i].compressed_size = csz;
                ctx->jobs[i].state = JOB_COMPLETE;
                (*ctx->completed)++;
                printf("[server] Got chunk %d/%d from %s (%zu→%zu B, %.1f%%)\n",
                       ctx->jobs[i].chunk_index + 1, ctx->jobs[i].total_chunks,
                       peer_ip, ctx->jobs[i].chunk_size, csz,
                       (double)csz * 100.0 / ctx->jobs[i].chunk_size);
                data = NULL; break;
            }
        }
        pthread_mutex_unlock(ctx->jlock);
        free(data);

        pthread_mutex_lock(&client_lock);
        for (int i = 0; i < client_count; i++)
            if (strcmp(clients[i].ip, peer_ip) == 0) clients[i].state = CLIENT_IDLE;
        pthread_mutex_unlock(&client_lock);
    }
    return NULL;
}

/* ── Reassemble compressed output ── */
static int reassemble(const char *filename, job_t *jobs, int cnt)
{
    char out[MAX_FILENAME_LEN + 4];
    snprintf(out, sizeof(out), "%s.gz", filename);
    FILE *fp = fopen(out, "wb");
    if (!fp) { perror("fopen output"); return -1; }
    for (int i = 0; i < cnt; i++) {
        for (int j = 0; j < cnt; j++) {
            if (jobs[j].chunk_index == i && jobs[j].result_data) {
                fwrite(jobs[j].result_data, 1, jobs[j].compressed_size, fp);
                break;
            }
        }
    }
    fclose(fp);
    printf("[server] Output: %s\n", out);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    char *cfgpath = CONFIG_PATH;
    int disc_wait = 5;
    int opt;
    while ((opt = getopt(argc, argv, "c:w:h")) != -1) {
        switch (opt) {
        case 'c': cfgpath = optarg; break;
        case 'w': disc_wait = atoi(optarg); break;
        default:
            fprintf(stderr, "Usage: %s [-c config] [-w wait_sec] <file ...>\n", argv[0]);
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "[server] No input files.\n"); return 1;
    }

    server_config_t cfg;
    parse_server_config(cfgpath, &cfg);

    char *rip = NULL;
    if (cfg.bind_address[0] == '\0' && cfg.interface[0]) {
        rip = getIPAddr(cfg.interface);
        strncpy(cfg.bind_address, rip, MAX_IP_LEN - 1);
    }

    printf("[server] Bind: %s:%d  Subnet: %s  Chunk: %zuMB\n",
           cfg.bind_address, cfg.port, cfg.subnet[0] ? cfg.subnet : "any", cfg.chunk_size_mb);

    int disc_fd = listenOnIP(cfg.bind_address, cfg.port);
    if (disc_fd < 0) { free(rip); return 1; }

    discovery_ctx_t dctx = { .cfg = &cfg, .listen_fd = disc_fd };
    pthread_t dtid;
    pthread_create(&dtid, NULL, discovery_thread, &dctx);

    printf("[server] Waiting %ds for clients...\n", disc_wait);
    sleep(disc_wait);

    if (client_count == 0) {
        fprintf(stderr, "[server] No clients found.\n");
        keep_running = 0; close(disc_fd); pthread_join(dtid, NULL); free(rip);
        return 1;
    }
    printf("[server] %d client(s) ready\n", client_count);

    int coll_fd = listenOnIP(cfg.bind_address, cfg.port + 1);
    if (coll_fd < 0) { keep_running = 0; close(disc_fd); pthread_join(dtid, NULL); free(rip); return 1; }

    for (int f = optind; f < argc && keep_running; f++) {
        printf("\n[server] ═══ %s ═══\n", argv[f]);
        job_t *jobs = NULL; int jcnt = 0;
        if (split_file_into_jobs(argv[f], cfg.chunk_size_mb * 1024 * 1024, &jobs, &jcnt) < 0) continue;
        printf("[server] %d chunk(s)\n", jcnt);

        int done = 0;
        pthread_mutex_t jlock = PTHREAD_MUTEX_INITIALIZER;
        collector_ctx_t cctx = { coll_fd, jobs, jcnt, &done, &jlock, &cfg };
        pthread_t ctid;
        pthread_create(&ctid, NULL, collector_thread, &cctx);

        int rr = 0;
        while (done < jcnt && keep_running) {
            pthread_mutex_lock(&jlock);
            for (int i = 0; i < jcnt; i++) {
                if (jobs[i].state == JOB_PENDING) {
                    int ci = find_idle_client(rr);
                    if (ci < 0) break;
                    set_client_state(ci, CLIENT_BUSY);
                    if (dispatch_job(&jobs[i], ci) == 0) rr = (ci + 1) % client_count;
                    else set_client_state(ci, CLIENT_DEAD);
                }
                if (jobs[i].state == JOB_ASSIGNED && cfg.retry_failed_jobs) {
                    if (time(NULL) - jobs[i].assigned_time > JOB_TIMEOUT_SECS) {
                        jobs[i].retries++;
                        if (jobs[i].retries >= MAX_RETRIES) jobs[i].state = JOB_FAILED;
                        else { jobs[i].state = JOB_PENDING; printf("[server] Retry chunk %d\n", jobs[i].chunk_index); }
                    }
                }
            }
            pthread_mutex_unlock(&jlock);
            usleep(200000);
        }
        pthread_join(ctid, NULL);

        int ok = 1;
        struct stat st; stat(argv[f], &st);
        size_t tc = 0;
        for (int i = 0; i < jcnt; i++) {
            if (jobs[i].state != JOB_COMPLETE) ok = 0;
            else tc += jobs[i].compressed_size;
        }
        if (ok) {
            reassemble(argv[f], jobs, jcnt);
            printf("[server] %s: %zu → %zu B (%.1f%%)\n", argv[f], (size_t)st.st_size, tc, (double)tc*100.0/st.st_size);
        } else fprintf(stderr, "[server] FAILED: %s\n", argv[f]);

        free_jobs(jobs, jcnt);
        pthread_mutex_destroy(&jlock);
    }

    printf("\n[server] Done.\n");
    keep_running = 0;
    close(disc_fd); close(coll_fd);
    pthread_join(dtid, NULL);
    free(rip);
    return 0;
}

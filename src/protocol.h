#ifndef DISTZIP_PROTOCOL_H
#define DISTZIP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

/*
 * distZIP Wire Protocol
 * ---------------------
 * All messages are newline-delimited text headers followed by optional binary
 * payload.
 *
 * SERVER → CLIENT:
 *   DISTZIP-JOB <job-uuid> <orig-filename> <chunk-idx> <total-chunks>
 * <chunk-size>\n [binary chunk data of chunk-size bytes]
 *
 * CLIENT → SERVER:
 *   DISTZIP-RESULT <job-uuid> <compressed-size>\n
 *   [compressed binary data of compressed-size bytes]
 *
 * SERVER → CLIENT (discovery):
 *   DISTZIP-PING\n
 *
 * CLIENT → SERVER (discovery response):
 *   DISTZIP-PONG <client-id>\n
 */

/* Protocol version */
#define DISTZIP_PROTO_VERSION 1

/* Message tags (text prefixes) */
#define TAG_JOB "DISTZIP-JOB"
#define TAG_RESULT "DISTZIP-RESULT"
#define TAG_PING "DISTZIP-PING"
#define TAG_PONG "DISTZIP-PONG"

/* Default ports */
#define DEFAULT_SERVER_PORT 9090
#define DEFAULT_CLIENT_PORT 9999

/* Limits */
#define MAX_HEADER_LEN 1024
#define MAX_FILENAME_LEN 1024
#define MAX_CLIENT_ID_LEN 128
#define MAX_UUID_STR_LEN 37
#define MAX_IP_LEN 64
#define NET_BUF_SIZE 65536 /* 64KB read/write buffer */

/* Message types */
typedef enum {
  MSG_JOB = 0,
  MSG_RESULT,
  MSG_PING,
  MSG_PONG,
  MSG_UNKNOWN
} msg_type_t;

/* Identify message type from header line */
static inline msg_type_t parse_msg_type(const char *header) {
  if (!header)
    return MSG_UNKNOWN;
  if (strncmp(header, TAG_JOB, sizeof(TAG_JOB) - 1) == 0)
    return MSG_JOB;
  if (strncmp(header, TAG_RESULT, sizeof(TAG_RESULT) - 1) == 0)
    return MSG_RESULT;
  if (strncmp(header, TAG_PING, sizeof(TAG_PING) - 1) == 0)
    return MSG_PING;
  if (strncmp(header, TAG_PONG, sizeof(TAG_PONG) - 1) == 0)
    return MSG_PONG;
  return MSG_UNKNOWN;
}

#endif /* DISTZIP_PROTOCOL_H */

/*
 * cadence_msg.h — Cadence QNX service wire protocol.
 *
 * Shared by the daemon (src/daemon) and the client (src/client).
 * All messages are plain structs exchanged through the QNX microkernel
 * message passing (MsgSend / MsgReceive) over the QNX name service.
 *
 * Every message starts with a cadence_hdr so that a mismatching peer is
 * detected instead of being interpreted as garbage.
 */
#ifndef CADENCE_MSG_H
#define CADENCE_MSG_H

#include <stdint.h>

/*
 * Name under which the daemon registers itself in the QNX name service.
 * name_attach() publishes it under /dev/name/local/<name>, the client
 * resolves it with name_open(<name>).
 */
#define CADENCE_SERVICE_NAME   "cadence-llm"

/* Full path of the name once published by the QNX name service. */
#define CADENCE_SERVICE_PATH   "/dev/name/local/cadence-llm"

/* Protocol version, bumped on any incompatible change. */
#define CADENCE_PROTO_VERSION  1u

/* 'CADE' in big endian, checked on every message. */
#define CADENCE_MAGIC          0x43414445u

/* Maximum size of the user prompt carried by a request (bytes). */
#define CADENCE_MAX_PROMPT     2048

/* Maximum size of an error message (bytes). */
#define CADENCE_MAX_TEXT       256

/* Maximum size of one streamed token (bytes). */
#define CADENCE_MAX_TOKEN_TEXT 128

/*
 * Requests: client -> server.
 * The values are above _IO_MAX: on a dispatch channel the types below
 * _IO_MAX are reserved for the system messages of the resource manager.
 */
enum cadence_req_type {
    CADENCE_REQ_GENERATE = 0x1001, /* start a generation, tokens are pushed back */
    CADENCE_REQ_PING     = 0x1002, /* liveness probe */
    CADENCE_REQ_STATUS   = 0x1003, /* scheduler / affinity report */
    CADENCE_REQ_CANCEL   = 0x1004  /* cancel the running generation */
};

/* Responses: server -> client. */
enum cadence_rsp_type {
    CADENCE_RSP_ACCEPTED = 0x2001, /* request accepted, generation is queued */
    CADENCE_RSP_TOKEN    = 0x2002, /* one generated token */
    CADENCE_RSP_DONE     = 0x2003, /* generation finished */
    CADENCE_RSP_ERROR    = 0x2004, /* request failed */
    CADENCE_RSP_PONG     = 0x2005, /* answer to CADENCE_REQ_PING */
    CADENCE_RSP_STATUS   = 0x2006  /* answer to CADENCE_REQ_STATUS */
};

/* Why a generation stopped. */
enum cadence_finish_reason {
    CADENCE_FINISH_EOS       = 0, /* end of sequence reached */
    CADENCE_FINISH_LENGTH    = 1, /* max_tokens reached */
    CADENCE_FINISH_CANCELLED = 2, /* cancelled by the client */
    CADENCE_FINISH_ERROR     = 3  /* inference error */
};

/* Common header, first field of every message. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t flags;
} cadence_hdr;

/* Request carrying the user prompt. */
typedef struct {
    cadence_hdr hdr;
    int32_t  max_tokens;  /* 0 = server default */
    int32_t  client_pid;  /* where the token stream is pushed */
    int32_t  client_chid; /* channel of the client notification queue */
    uint32_t seed;        /* 0 = server default (deterministic seed) */
    char     prompt[CADENCE_MAX_PROMPT];
} cadence_req_generate;

/* Request without payload (ping / status / cancel). */
typedef struct {
    cadence_hdr hdr;
} cadence_req_simple;

/* Answer to CADENCE_REQ_GENERATE: the job is queued on the inference thread. */
typedef struct {
    cadence_hdr hdr;
    uint32_t job_id;
} cadence_rsp_accepted;

/* One streamed token. */
typedef struct {
    cadence_hdr hdr;
    uint32_t seq;        /* token index in the stream */
    int32_t  token_id;   /* llama token id */
    int32_t  n_decoded;  /* tokens decoded so far */
    double   elapsed_ms; /* server side time since the start of the generation */
    char     text[CADENCE_MAX_TOKEN_TEXT];
} cadence_rsp_token;

/* Last message of a generation, carries the throughput. */
typedef struct {
    cadence_hdr hdr;
    uint32_t seq;
    uint32_t n_tokens;
    int32_t  finish;        /* cadence_finish_reason */
    double   total_ms;      /* server side generation time */
    double   ms_per_token;
    double   tokens_per_sec;
} cadence_rsp_done;

/* Error report. */
typedef struct {
    cadence_hdr hdr;
    int32_t code; /* errno like value, 0 = unspecified */
    char    message[CADENCE_MAX_TEXT];
} cadence_rsp_error;

/* Answer to CADENCE_REQ_STATUS: real-time configuration of the service. */
typedef struct {
    cadence_hdr hdr;
    int32_t  policy;   /* SCHED_FIFO */
    int32_t  priority; /* priority of the inference thread */
    uint32_t runmask;  /* CPU affinity mask of the inference thread */
    int32_t  n_threads;/* llama.cpp worker threads */
    int32_t  ctx_size; /* context size in tokens */
    int32_t  max_tokens;
    char     model[128];
} cadence_rsp_status;

#endif /* CADENCE_MSG_H */

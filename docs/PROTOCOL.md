# Cadence QNX service protocol

`cadenced` is a native QNX 8.0 resource manager: it owns a GGUF LLM and
serves it to any QNX process through the microkernel message passing.
There is no TCP socket, no shared memory and no file based RPC: every
exchange is a message.

* service name (QNX name service): `cadence-llm`
* full path once published: `/dev/name/local/cadence-llm`
* client side resolution: `name_open("cadence-llm", 0)`
* server side publication: `name_attach(NULL, "cadence-llm", 0)`

The shared description of the messages lives in `src/common/cadence_msg.h`
and is included by both the daemon (`src/daemon/cadenced.c`) and the
client (`src/client/cadence-cli.c`).

## Transport

| Step | Call | Direction |
|------|------|-----------|
| publish the service | `name_attach(NULL, name, 0)` | daemon |
| connect | `name_open(name, 0)` → coid | client |
| send a request | `MsgSend(coid, &req, sizeof(req), &reply, sizeof(reply))` | client → daemon |
| receive a request | `rcvid = MsgReceive(chid, &msg, sizeof(msg), &info)` | daemon |
| answer a request | `MsgReply(rcvid, EOK, &reply, sizeof(reply))` | daemon → client |
| stream the tokens | `MsgSend(coid, &token, sizeof(token), NULL, 0)` | daemon → client |
| receive the tokens | `rcvid = MsgReceive(client_chid, &msg, sizeof(msg), NULL)` | client |

A generation is **asynchronous**: the client sends `CADENCE_REQ_GENERATE`
and gets an immediate `CADENCE_RSP_ACCEPTED` acknowledgement. The daemon
then connects to the notification channel advertised by the client
(`ConnectAttach`) and pushes one message per token, then a final
`CADENCE_RSP_DONE`. The requests are serialized on the single inference
thread of the service, several clients may be connected at the same time.

## Common header

Every message starts with:

```c
typedef struct {
    uint32_t magic;    /* 0x43414445 ('CADE') */
    uint32_t version;  /* protocol version, currently 1 */
    uint32_t type;     /* request or response type */
    uint32_t flags;    /* reserved, 0 */
} cadence_hdr;
```

A message whose `magic` does not match is rejected with
`CADENCE_RSP_ERROR` (`EPROTO`).

## Requests (client → daemon)

| Type | Value | Payload | Answer |
|------|-------|---------|--------|
| `CADENCE_REQ_GENERATE` | 1 | `cadence_req_generate` | `CADENCE_RSP_ACCEPTED`, then the token stream |
| `CADENCE_REQ_PING` | 2 | `cadence_req_simple` | `CADENCE_RSP_PONG` |
| `CADENCE_REQ_STATUS` | 3 | `cadence_req_simple` | `CADENCE_RSP_STATUS` |
| `CADENCE_REQ_CANCEL` | 4 | `cadence_req_simple` | `CADENCE_RSP_PONG` |

```c
typedef struct {
    cadence_hdr hdr;
    int32_t  max_tokens;  /* 0 = server default */
    int32_t  client_pid;  /* where the token stream must be pushed */
    int32_t  client_chid; /* notification channel of the client */
    uint32_t seed;        /* 0 = server default */
    char     prompt[2048];
} cadence_req_generate;
```

`client_pid` and `client_chid` are mandatory: the daemon attaches to that
channel with `ConnectAttach(0, client_pid, client_chid, 0, 0)` and pushes
the messages there.

## Responses (daemon → client)

| Type | Value | Payload |
|------|-------|---------|
| `CADENCE_RSP_ACCEPTED` | 16 | `cadence_rsp_accepted` |
| `CADENCE_RSP_TOKEN` | 17 | `cadence_rsp_token` |
| `CADENCE_RSP_DONE` | 18 | `cadence_rsp_done` |
| `CADENCE_RSP_ERROR` | 19 | `cadence_rsp_error` |
| `CADENCE_RSP_PONG` | 20 | `cadence_rsp_status` |
| `CADENCE_RSP_STATUS` | 21 | `cadence_rsp_status` |

One token per message:

```c
typedef struct {
    cadence_hdr hdr;
    uint32_t seq;        /* index of the token in the stream */
    int32_t  token_id;   /* llama.cpp token id */
    int32_t  n_decoded;  /* tokens decoded so far */
    double   elapsed_ms; /* server side time since the start of the generation */
    char     text[128];  /* the text of the token */
} cadence_rsp_token;
```

Last message of a generation:

```c
typedef struct {
    cadence_hdr hdr;
    uint32_t seq;
    uint32_t n_tokens;
    int32_t  finish;        /* CADENCE_FINISH_EOS / _LENGTH / _CANCELLED / _ERROR */
    double   total_ms;      /* server side generation time */
    double   ms_per_token;
    double   tokens_per_sec;
} cadence_rsp_done;
```

Real-time configuration of the service (`CADENCE_RSP_STATUS`):

```c
typedef struct {
    cadence_hdr hdr;
    int32_t  policy;    /* SCHED_FIFO */
    int32_t  priority;  /* priority of the inference thread */
    uint32_t runmask;   /* CPU affinity mask of the inference thread */
    int32_t  n_threads; /* llama.cpp worker threads */
    int32_t  ctx_size;  /* context size in tokens */
    int32_t  max_tokens;
    char     model[128];
} cadence_rsp_status;
```

## Sequence of a generation

```
client                                   daemon
  |  name_open("cadence-llm")               |
  |---------------------------------------->|
  |  ChannelCreate() -> chid                |
  |  MsgSend(GENERATE, pid, chid, prompt)   |
  |---------------------------------------->|
  |  MsgReply(ACCEPTED)                     |
  |<----------------------------------------|
  |  MsgReceive(chid) -> TOKEN              |  ConnectAttach(client)
  |<----------------------------------------|  MsgSend(TOKEN)
  |  MsgReceive(chid) -> TOKEN              |
  |<----------------------------------------|  MsgSend(TOKEN)
  |  ...                                     |
  |  MsgReceive(chid) -> DONE               |  MsgSend(DONE)
  |<----------------------------------------|
  |  name_close() / ChannelDestroy()        |  ConnectDetach()
```

## Real-time behaviour

* the inference thread runs with `SCHED_FIFO` and a low priority (20 by
  default), so it never starves the rest of the system;
* its CPU affinity is pinned with `ThreadCtl(_NTO_TCTL_RUNMASK)`, the mask
  is reported by `CADENCE_RSP_STATUS`;
* the requests are serialized: one generation at a time on the inference
  thread, the dispatch thread stays responsive;
* `SIGTERM` / `SIGINT` stop the service: the name is detached, the model
  and the context are released.

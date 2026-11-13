/*
 * cadenced.c — Cadence QNX native LLM service (microkernel resource manager).
 *
 * The daemon owns the GGUF model and serves it to any QNX process through
 * the native message passing of the microkernel: the name service
 * (name_attach / name_open) plus MsgSend / MsgReceive / MsgReply.
 * No TCP socket, no shared memory: everything is a message.
 *
 * Architecture:
 *   - the main thread runs the dispatch loop (one request at a time);
 *   - a dedicated inference thread (SCHED_FIFO, low priority, CPU pinned)
 *     runs llama.cpp and pushes the tokens back to the client channel;
 *   - the requests are serialized on that single inference thread.
 */
#define _QNX_SOURCE 1

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/sched.h>
#include <time.h>
#include <unistd.h>

#include "cadence_llm.h"
#include "cadence_log.h"
#include "cadence_msg.h"
#include "cadence_opts.h"
#include "cadence_reply.h"

#define CADENCE_PROMPT_BUF 8192

/* Set by the signal handler, checked by the dispatch loop. */
static volatile sig_atomic_t g_stop = 0;

/* Set by a CADENCE_REQ_CANCEL request, checked by the token callback. */
static volatile int g_cancel = 0;

typedef struct {
    int            client_pid;
    int            client_chid;
    int            coid;
    uint32_t       seq;
    volatile int   cancel;
    int            client_gone;
} cadence_job_t;

typedef struct {
    cadence_llm_t *    llm;
    name_attach_t *    attach;
    int                chid;
    cadence_opts_t     opt;
    uint32_t           runmask;
    pthread_t          worker;
    int                worker_started;
    pthread_mutex_t    lock;
    pthread_cond_t     cond;
    int                job_pending;
    cadence_req_generate job;
} cadence_daemon_t;

static void on_signal(int sig)
{
    (void) sig;
    g_stop = 1; /* the dispatch loop leaves on EINTR */
}

/* Report a fatal fault instead of dying silently. */
static void on_fault(int sig)
{
    char buf[64];
    int n;

    n = snprintf(buf, sizeof(buf), "cadenced: FATAL fault, signal %d\n", sig);
    (void) write(STDERR_FILENO, buf, n);
    _exit(3);
}

static int install_handlers(void)
{
    struct sigaction sa;
    struct sigaction fa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: MsgReceive has to return EINTR */

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return -1;
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return -1;
    }

    memset(&fa, 0, sizeof(fa));
    fa.sa_handler = on_fault;
    sigemptyset(&fa.sa_mask);
    fa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &fa, NULL);
    sigaction(SIGBUS, &fa, NULL);
    sigaction(SIGILL, &fa, NULL);
    sigaction(SIGFPE, &fa, NULL);
    sigaction(SIGABRT, &fa, NULL);

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* Push one message to the notification channel of a client. */
static int push_to_client(cadence_job_t * job, const void * msg, size_t len)
{
    int rc;

    if (job->coid < 0) {
        return -1;
    }
    /*
     * MsgSendnc: the inference thread never waits for the reply of a
     * token, the client answers with MsgReply on its own channel.
     */
    rc = MsgSendnc(job->coid, msg, len, NULL, 0);
    if (rc == -1) {
        job->client_gone = 1;
        return -1;
    }
    return 0;
}

/* Called by llama.cpp for every generated token. */
static int on_token(void * user_data, const char * text, int token_id,
                    int n_decoded, double elapsed_ms)
{
    cadence_job_t * job = (cadence_job_t *) user_data;
    cadence_rsp_token rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_TOKEN;
    rsp.seq         = job->seq++;
    rsp.token_id    = token_id;
    rsp.n_decoded   = n_decoded;
    rsp.elapsed_ms  = elapsed_ms;
    snprintf(rsp.text, sizeof(rsp.text), "%s", text);

    if (push_to_client(job, &rsp, sizeof(rsp)) != 0) {
        return 1; /* the client vanished: stop the generation */
    }
    return (job->cancel || g_cancel) ? 1 : 0;
}

/* Send an error message to the client and close the connection. */
static void push_error(cadence_job_t * job, int code, const char * message)
{
    cadence_rsp_error rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_ERROR;
    rsp.code        = code;
    snprintf(rsp.message, sizeof(rsp.message), "%s", message);
    push_to_client(job, &rsp, sizeof(rsp));
}

/* Fill the final message of a generation (throughput). */
static void fill_done(cadence_rsp_done * done, const cadence_job_t * job,
                      int n_tokens, int max_tokens, double total_ms)
{
    memset(done, 0, sizeof(*done));
    done->hdr.magic   = CADENCE_MAGIC;
    done->hdr.version = CADENCE_PROTO_VERSION;
    done->hdr.type    = CADENCE_RSP_DONE;
    done->seq         = job->seq;
    done->n_tokens    = (n_tokens > 0) ? (uint32_t) n_tokens : 0;
    done->total_ms    = total_ms;
    if (done->n_tokens > 0) {
        done->ms_per_token   = total_ms / (double) done->n_tokens;
        done->tokens_per_sec = (total_ms > 0.0) ? (done->n_tokens * 1000.0 / total_ms) : 0.0;
    }
    if (job->cancel || g_cancel) {
        done->finish = CADENCE_FINISH_CANCELLED;
    } else if (n_tokens < 0) {
        done->finish = CADENCE_FINISH_ERROR;
    } else if (n_tokens >= max_tokens) {
        done->finish = CADENCE_FINISH_LENGTH;
    } else {
        done->finish = CADENCE_FINISH_EOS;
    }
}

/* Run one generation and stream the tokens to the client. */
static void run_generation(cadence_daemon_t * d, const cadence_req_generate * req)
{
    cadence_job_t job;
    cadence_rsp_done done;
    char prompt[CADENCE_PROMPT_BUF];
    char err[256];
    double t0;
    int n_tokens;
    int max_tokens;

    memset(&job, 0, sizeof(job));
    job.client_pid  = req->client_pid;
    job.client_chid = req->client_chid;
    job.coid        = -1;
    job.seq         = 1;
    g_cancel        = 0;

    if (job.client_pid <= 0 || job.client_chid <= 0) {
        cadence_log("WARN", "generate request without client channel, ignored");
        return;
    }
    job.coid = ConnectAttach(0, (pid_t) job.client_pid, job.client_chid, 0, 0);
    if (job.coid == -1) {
        cadence_log("WARN", "ConnectAttach to client %d chid %d failed: %s",
                    job.client_pid, job.client_chid, strerror(errno));
        return;
    }

    max_tokens = req->max_tokens > 0 ? req->max_tokens : d->opt.max_tokens;
    t0 = cadence_now_ms();

    if (cadence_llm_build_prompt(d->llm, req->prompt, prompt, sizeof(prompt), err, sizeof(err)) != 0) {
        push_error(&job, EINVAL, err);
        ConnectDetach(job.coid);
        return;
    }

    n_tokens = cadence_llm_generate(d->llm, prompt, max_tokens,
                                     req->seed ? req->seed : d->opt.seed,
                                     on_token, &job);

    fill_done(&done, &job, n_tokens, max_tokens, cadence_now_ms() - t0);
    push_to_client(&job, &done, sizeof(done));
    ConnectDetach(job.coid);

    cadence_log("INFO", "generation done: %u tokens in %.0f ms (%.2f tok/s)",
                done.n_tokens, done.total_ms, done.tokens_per_sec);
}

/* Inference thread: one job at a time, tokens are pushed to the client. */
static void * worker_main(void * arg)
{
    cadence_daemon_t * d = (cadence_daemon_t *) arg;
    uint64_t mask = (uint64_t) 1 << d->opt.cpu;

    /* ThreadCtl applies to the calling thread: pin ourselves.
     * The runmask is passed by value, disguised as a pointer. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *) (uintptr_t) mask) == -1) {
        cadence_log("WARN", "cannot pin the inference thread on CPU %d: %s",
                    d->opt.cpu, strerror(errno));
    } else {
        d->runmask = (uint32_t) mask;
        cadence_log("INFO", "inference thread: SCHED_FIFO prio %d, pinned on CPU %d",
                    d->opt.prio, d->opt.cpu);
    }

    for (;;) {
        cadence_req_generate job;

        pthread_mutex_lock(&d->lock);
        while (!d->job_pending && !g_stop) {
            pthread_cond_wait(&d->cond, &d->lock);
        }
        if (!d->job_pending) {
            pthread_mutex_unlock(&d->lock);
            break;
        }
        job = d->job;
        d->job_pending = 0;
        pthread_mutex_unlock(&d->lock);

        run_generation(d, &job);
    }
    return NULL;
}

/* Start the inference thread: SCHED_FIFO, low priority, CPU pinned. */
static int start_worker(cadence_daemon_t * d)
{
    pthread_attr_t attr;
    struct sched_param sp;
    int rc;

    pthread_attr_init(&attr);
    sp.sched_priority = d->opt.prio;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);

    rc = pthread_create(&d->worker, &attr, worker_main, d);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        cadence_log("ERROR", "cannot create the inference thread: %s", strerror(rc));
        return -1;
    }
    d->worker_started = 1;

    cadence_log("INFO", "inference thread started: SCHED_FIFO prio %d, CPU %d",
                d->opt.prio, d->opt.cpu);
    return 0;
}

/* Queue a generation request on the inference thread. */
static void handle_generate(cadence_daemon_t * d, rcvid_t rcvid,
                            const cadence_req_generate * req)
{
    cadence_rsp_accepted rsp;

    pthread_mutex_lock(&d->lock);
    d->job         = *req;
    d->job_pending = 1;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_ACCEPTED;
    rsp.job_id      = 1;
    MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
    cadence_log("INFO", "generate request queued (%d chars)", (int) strlen(req->prompt));
}

/* Dispatch one received message. */
static void handle_request(cadence_daemon_t * d, rcvid_t rcvid,
                           const cadence_hdr * hdr, const void * body)
{
    switch (hdr->type) {
    case CADENCE_REQ_GENERATE:
        handle_generate(d, rcvid, (const cadence_req_generate *) body);
        break;
    case CADENCE_REQ_PING:
        cadence_reply_pong(rcvid);
        break;
    case CADENCE_REQ_STATUS: {
        cadence_status_t status;
        memset(&status, 0, sizeof(status));
        status.n_threads  = d->opt.n_threads;
        status.ctx_size   = d->opt.n_ctx;
        status.max_tokens = d->opt.max_tokens;
        status.priority   = d->opt.prio;
        status.runmask    = d->runmask;
        snprintf(status.model, sizeof(status.model), "%.127s", d->opt.model);
        cadence_reply_status(rcvid, &status);
        break;
    }
    case CADENCE_REQ_CANCEL:
        g_cancel = 1;
        cadence_reply_pong(rcvid);
        break;
    default:
        cadence_reply_error(rcvid, EINVAL, "unknown request type");
        break;
    }
}

/* Main dispatch loop: receive the messages sent to our name. */
static void dispatch_loop(cadence_daemon_t * d)
{
    union {
        struct _pulse        pulse;
        cadence_hdr          hdr;
        cadence_req_generate gen;
        cadence_req_simple   simple;
    } msg;
    struct _msg_info info;
    rcvid_t rcvid;

    while (!g_stop) {
        memset(&msg, 0, sizeof(msg));
        rcvid = MsgReceive(d->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue; /* a signal: check g_stop */
            }
            cadence_log("ERROR", "MsgReceive failed: %s", strerror(errno));
            break;
        }
        if (rcvid == 0) {
            /* pulse: a client disconnected */
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
            }
            continue;
        }
        if (msg.hdr.type <= _IO_MAX) {
            MsgError(rcvid, ENOSYS); /* system message: not for us */
            continue;
        }
        if (msg.hdr.magic != CADENCE_MAGIC) {
            cadence_reply_error(rcvid, EPROTO, "bad magic");
            continue;
        }
        if (msg.hdr.version != CADENCE_PROTO_VERSION) {
            cadence_reply_error(rcvid, EPROTO, "unsupported protocol version");
            continue;
        }
        handle_request(d, rcvid, &msg.hdr, &msg);
    }
}

/* Load the model, publish the name and serve until SIGTERM. */
static int run_service(cadence_daemon_t * d)
{
    char err[256];

    cadence_log("INFO", "loading model '%s' (ctx=%d, threads=%d)",
                d->opt.model, d->opt.n_ctx, d->opt.n_threads);
    d->llm = cadence_llm_load(d->opt.model, d->opt.n_ctx, d->opt.n_threads, err, sizeof(err));
    if (!d->llm) {
        cadence_log("FATAL", "%s", err);
        return 1;
    }
    cadence_log("INFO", "model loaded");

    d->attach = name_attach(NULL, d->opt.name, 0);
    if (!d->attach) {
        cadence_log("FATAL", "name_attach('%s') failed: %s", d->opt.name, strerror(errno));
        cadence_llm_free(d->llm);
        return 1;
    }
    d->chid = d->attach->chid;

    if (start_worker(d) != 0) {
        name_detach(d->attach, 0);
        cadence_llm_free(d->llm);
        return 1;
    }

    cadence_log("INFO", "cadenced ready on /dev/name/local/%s (pid %d)",
                d->opt.name, (int) getpid());
    dispatch_loop(d);

    cadence_log("INFO", "shutting down");
    g_stop = 1;
    pthread_mutex_lock(&d->lock);
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    if (d->worker_started) {
        pthread_join(d->worker, NULL);
    }
    name_detach(d->attach, 0);
    cadence_llm_free(d->llm);
    cadence_log("INFO", "bye");
    return 0;
}

int main(int argc, char ** argv)
{
    cadence_daemon_t d;
    int rc;

    memset(&d, 0, sizeof(d));
    rc = cadence_parse_args(argc, argv, &d.opt);
    if (rc == 2) {
        cadence_usage(argv[0]);
        return 0;
    }
    if (rc != 0) {
        cadence_usage(argv[0]);
        return 1;
    }
    if (install_handlers() != 0) {
        cadence_log("FATAL", "cannot install the signal handlers: %s", strerror(errno));
        return 1;
    }

    pthread_mutex_init(&d.lock, NULL);
    pthread_cond_init(&d.cond, NULL);

    return run_service(&d);
}

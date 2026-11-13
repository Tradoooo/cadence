/*
 * cadenced — Cadence QNX native LLM service (microkernel resource manager).
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
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
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
#include "cadence_opts.h"
#include "cadence_msg.h"
#include "cadence_sched.h"
#include "cadence_util.h"

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

/* Push one message to the notification channel of a client. */
static int push_to_client(cadence_job_t * job, const void * msg, size_t len)
{
    int rc;

    if (job->coid < 0) {
        return -1;
    }
    rc = MsgSend(job->coid, msg, len, NULL, 0);
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
        cadence_rsp_error rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.hdr.magic   = CADENCE_MAGIC;
        rsp.hdr.version = CADENCE_PROTO_VERSION;
        rsp.hdr.type    = CADENCE_RSP_ERROR;
        rsp.code        = EINVAL;
        snprintf(rsp.message, sizeof(rsp.message), "%s", err);
        push_to_client(&job, &rsp, sizeof(rsp));
        ConnectDetach(job.coid);
        return;
    }

    n_tokens = cadence_llm_generate(d->llm, prompt, max_tokens,
                                     req->seed ? req->seed : d->opt.seed,
                                     on_token, &job);

    memset(&done, 0, sizeof(done));
    done.hdr.magic   = CADENCE_MAGIC;
    done.hdr.version = CADENCE_PROTO_VERSION;
    done.hdr.type    = CADENCE_RSP_DONE;
    done.seq         = job.seq;
    done.n_tokens    = (n_tokens > 0) ? (uint32_t) n_tokens : 0;
    done.total_ms    = cadence_now_ms() - t0;
    done.finish      = job.cancel ? CADENCE_FINISH_CANCELLED
                     : (n_tokens < 0) ? CADENCE_FINISH_ERROR
                     : (n_tokens >= max_tokens) ? CADENCE_FINISH_LENGTH
                     : CADENCE_FINISH_EOS;
    if (done.n_tokens > 0) {
        done.ms_per_token   = done.total_ms / (double) done.n_tokens;
        done.tokens_per_sec = (done.total_ms > 0.0) ? (done.n_tokens * 1000.0 / done.total_ms) : 0.0;
    }
    push_to_client(&job, &done, sizeof(done));
    ConnectDetach(job.coid);

    cadence_log("INFO", "generation done: %u tokens in %.0f ms (%.2f tok/s)",
            done.n_tokens, done.total_ms, done.tokens_per_sec);
}

/* Inference thread: one job at a time, tokens are pushed to the client. */
static void * worker_main(void * arg)
{
    cadence_daemon_t * d = (cadence_daemon_t *) arg;

    /* Pin ourselves on the dedicated CPU: no migration, no cache eviction. */
    if (cadence_pin_thread(d->opt.cpu) == 0) {
        d->runmask = (uint32_t) ((uint64_t) 1 << d->opt.cpu);
        cadence_log("INFO", "inference thread: SCHED_FIFO prio %d, pinned on CPU %d",
                    d->opt.prio, d->opt.cpu);
    }

    for (;;) {
        cadence_req_generate job;

        pthread_mutex_lock(&d->lock);
        while (!d->job_pending && !cadence_stop_requested()) {
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
    if (cadence_create_worker(&d->worker, d->opt.prio, worker_main, d) != 0) {
        return -1;
    }
    d->worker_started = 1;

    cadence_log("INFO", "inference thread started: SCHED_FIFO prio %d, CPU %d",
            d->opt.prio, d->opt.cpu);
    return 0;
}

/* Answer a request with an error message. */
static void reply_error(rcvid_t rcvid, int code, const char * message)
{
    cadence_rsp_error rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_ERROR;
    rsp.code        = code;
    snprintf(rsp.message, sizeof(rsp.message), "%s", message);
    MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
}

/* Answer a request with the real-time configuration of the service. */
static void reply_status(cadence_daemon_t * d, rcvid_t rcvid)
{
    cadence_rsp_status rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_STATUS;
    rsp.policy      = SCHED_FIFO;
    rsp.priority    = d->opt.prio;
    rsp.runmask     = d->runmask;
    rsp.n_threads   = d->opt.n_threads;
    rsp.ctx_size    = d->opt.n_ctx;
    rsp.max_tokens  = d->opt.max_tokens;
    snprintf(rsp.model, sizeof(rsp.model), "%s", d->opt.model);
    MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
}

/* Queue a generation request on the inference thread. */
static void handle_generate(cadence_daemon_t * d, rcvid_t rcvid,
                            const cadence_req_generate * req)
{
    cadence_rsp_accepted rsp;

    pthread_mutex_lock(&d->lock);
    d->job        = *req;
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
    case CADENCE_REQ_PING: {
        cadence_rsp_status rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.hdr.magic   = CADENCE_MAGIC;
        rsp.hdr.version = CADENCE_PROTO_VERSION;
        rsp.hdr.type    = CADENCE_RSP_PONG;
        MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
        break;
    }
    case CADENCE_REQ_STATUS:
        reply_status(d, rcvid);
        break;
    case CADENCE_REQ_CANCEL:
        g_cancel = 1;
        {
            cadence_rsp_status rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.hdr.magic   = CADENCE_MAGIC;
            rsp.hdr.version = CADENCE_PROTO_VERSION;
            rsp.hdr.type    = CADENCE_RSP_PONG;
            MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
        }
        break;
    default:
        reply_error(rcvid, EINVAL, "unknown request type");
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

    while (!cadence_stop_requested()) {
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
            reply_error(rcvid, EPROTO, "bad magic");
            continue;
        }
        if (msg.hdr.version != CADENCE_PROTO_VERSION) {
            reply_error(rcvid, EPROTO, "unsupported protocol version");
            continue;
        }
        handle_request(d, rcvid, &msg.hdr, &msg);
    }
}

int main(int argc, char ** argv)
{
    cadence_daemon_t d;
    cadence_opts_t opt;
    char err[256];
    int rc;

    rc = cadence_parse_args(argc, argv, &opt);
    if (rc == 2) {
        cadence_usage(argv[0]);
        return 0;
    }
    if (rc != 0) {
        cadence_usage(argv[0]);
        return 1;
    }
    if (cadence_install_handlers() != 0) {
        cadence_log("FATAL", "cannot install the signal handlers: %s", strerror(errno));
        return 1;
    }

    /*
     * Real-time preparation, before anything else is allocated:
     *   - lock the address space so that no page of the model or of the KV
     *     cache can be swapped out in the middle of a generation;
     *   - keep the dispatch thread at a fixed low priority, so that a
     *     high-priority client cannot make the service hog the CPU.
     */
    cadence_lock_memory();
    cadence_set_thread_prio(opt.prio);

    memset(&d, 0, sizeof(d));
    d.opt = opt;
    pthread_mutex_init(&d.lock, NULL);
    pthread_cond_init(&d.cond, NULL);

    cadence_log("INFO", "loading model '%s' (ctx=%d, threads=%d)", opt.model, opt.n_ctx, opt.n_threads);
    {
        long prefaulted = cadence_prefault_file(opt.model);
        if (prefaulted > 0) {
            cadence_log("INFO", "model pre-faulted: %ld bytes read from the page cache",
                        prefaulted);
        }
    }
    d.llm = cadence_llm_load(opt.model, opt.n_ctx, opt.n_threads, err, sizeof(err));
    if (!d.llm) {
        cadence_log("FATAL", "%s", err);
        return 1;
    }
    cadence_log("INFO", "model loaded");

    d.attach = name_attach(NULL, opt.name, 0);
    if (!d.attach) {
        cadence_log("FATAL", "name_attach('%s') failed: %s", opt.name, strerror(errno));
        cadence_llm_free(d.llm);
        return 1;
    }
    d.chid = d.attach->chid;

    if (start_worker(&d) != 0) {
        name_detach(d.attach, 0);
        cadence_llm_free(d.llm);
        return 1;
    }

    cadence_log("INFO", "cadenced ready on /dev/name/local/%s (pid %d)", opt.name, (int) getpid());
    dispatch_loop(&d);

    cadence_log("INFO", "shutting down");
    cadence_request_stop();
    pthread_mutex_lock(&d.lock);
    pthread_cond_broadcast(&d.cond);
    pthread_mutex_unlock(&d.lock);
    if (d.worker_started) {
        pthread_join(d.worker, NULL);
    }
    name_detach(d.attach, 0);
    cadence_llm_free(d.llm);
    cadence_log("INFO", "bye");
    return 0;
}

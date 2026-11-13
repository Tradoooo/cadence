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
#include "cadence_msg.h"

#define CADENCE_DEFAULT_NAME    "cadence-llm"
#define CADENCE_DEFAULT_MODEL   "/proc/boot/model.gguf"
#define CADENCE_DEFAULT_CTX     4096
#define CADENCE_DEFAULT_THREADS 4
#define CADENCE_DEFAULT_PRIO    20
#define CADENCE_DEFAULT_MAXTOK  128
#define CADENCE_PROMPT_BUF      8192

/* Set by the signal handler, checked by the dispatch loop. */
static volatile sig_atomic_t g_stop = 0;

/* Set by a CADENCE_REQ_CANCEL request, checked by the token callback. */
static volatile int g_cancel = 0;

typedef struct {
    char     model[512];
    char     name[64];
    int      n_ctx;
    int      n_threads;
    int      prio;
    int      cpu;
    int      max_tokens;
    uint32_t seed;
} cadence_opts_t;

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

static void log_msg(const char * level, const char * fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%lld.%03ld] cadenced: %-5s ",
            (long long) ts.tv_sec, ts.tv_nsec / 1000000, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1000000.0;
}

static void usage(const char * prog)
{
    printf("Usage: %s [options]\n"
           "  -m, --model PATH     GGUF model to serve (default %s)\n"
           "  -n, --name NAME      QNX name service entry (default %s)\n"
           "  -c, --ctx N          context size in tokens (default %d)\n"
           "  -t, --threads N      llama.cpp worker threads (default %d)\n"
           "  -P, --prio N         SCHED_FIFO priority of the inference thread (default %d)\n"
           "  -C, --cpu N          CPU the inference thread is pinned to (default 0)\n"
           "  -M, --max-tokens N   default maximum number of tokens (default %d)\n"
           "  -s, --seed N         sampling seed (0 = random)\n"
           "  -h, --help           this help\n",
           prog, CADENCE_DEFAULT_MODEL, CADENCE_DEFAULT_NAME, CADENCE_DEFAULT_CTX,
           CADENCE_DEFAULT_THREADS, CADENCE_DEFAULT_PRIO, CADENCE_DEFAULT_MAXTOK);
}

/* Parse the command line: 0 on success, 1 on error, 2 when --help was asked. */
static int parse_args(int argc, char ** argv, cadence_opts_t * opt)
{
    int i;

    memset(opt, 0, sizeof(*opt));
    snprintf(opt->model, sizeof(opt->model), "%s", CADENCE_DEFAULT_MODEL);
    snprintf(opt->name, sizeof(opt->name), "%s", CADENCE_DEFAULT_NAME);
    opt->n_ctx      = CADENCE_DEFAULT_CTX;
    opt->n_threads  = CADENCE_DEFAULT_THREADS;
    opt->prio       = CADENCE_DEFAULT_PRIO;
    opt->cpu        = 0;
    opt->max_tokens = CADENCE_DEFAULT_MAXTOK;

    for (i = 1; i < argc; i++) {
        const char * a = argv[i];
        const char * v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            return 2;
        }
        if (!v) {
            fprintf(stderr, "cadenced: missing value for option '%s'\n", a);
            return 1;
        }
        if (!strcmp(a, "-m") || !strcmp(a, "--model")) {
            snprintf(opt->model, sizeof(opt->model), "%s", v);
        } else if (!strcmp(a, "-n") || !strcmp(a, "--name")) {
            snprintf(opt->name, sizeof(opt->name), "%s", v);
        } else if (!strcmp(a, "-c") || !strcmp(a, "--ctx")) {
            opt->n_ctx = atoi(v);
        } else if (!strcmp(a, "-t") || !strcmp(a, "--threads")) {
            opt->n_threads = atoi(v);
        } else if (!strcmp(a, "-P") || !strcmp(a, "--prio")) {
            opt->prio = atoi(v);
        } else if (!strcmp(a, "-C") || !strcmp(a, "--cpu")) {
            opt->cpu = atoi(v);
        } else if (!strcmp(a, "-M") || !strcmp(a, "--max-tokens")) {
            opt->max_tokens = atoi(v);
        } else if (!strcmp(a, "-s") || !strcmp(a, "--seed")) {
            opt->seed = (uint32_t) strtoul(v, NULL, 0);
        } else {
            fprintf(stderr, "cadenced: unknown option '%s'\n", a);
            return 1;
        }
        i++;
    }
    if (opt->n_ctx <= 0 || opt->n_threads <= 0 || opt->max_tokens <= 0) {
        fprintf(stderr, "cadenced: invalid numeric option\n");
        return 1;
    }
    return 0;
}

static void on_signal(int sig)
{
    (void) sig;
    g_stop = 1; /* the dispatch loop leaves on EINTR */
}

/*
 * Put the calling thread in SCHED_FIFO.
 *
 * It is called on the main thread before the llama.cpp context is built:
 * the llama.cpp worker threads are created at that moment and they inherit
 * the scheduling of their creator. They must not stay SCHED_OTHER, the
 * SCHED_FIFO inference thread would starve them and the generation would
 * stall (priority inversion).
 */
static void set_fifo(int prio)
{
    struct sched_param sp;

    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        log_msg("WARN", "cannot set SCHED_FIFO prio %d on the main thread", prio);
    }
}

static int install_handlers(void)
{
    struct sigaction sa;

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
        log_msg("WARN", "generate request without client channel, ignored");
        return;
    }
    job.coid = ConnectAttach(0, (pid_t) job.client_pid, job.client_chid, 0, 0);
    if (job.coid == -1) {
        log_msg("WARN", "ConnectAttach to client %d chid %d failed: %s",
                job.client_pid, job.client_chid, strerror(errno));
        return;
    }

    max_tokens = req->max_tokens > 0 ? req->max_tokens : d->opt.max_tokens;
    t0 = now_ms();

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
    done.total_ms    = now_ms() - t0;
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

    log_msg("INFO", "generation done: %u tokens in %.0f ms (%.2f tok/s)",
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
        log_msg("WARN", "cannot pin the inference thread on CPU %d: %s",
                d->opt.cpu, strerror(errno));
    } else {
        d->runmask = (uint32_t) mask;
        log_msg("INFO", "inference thread: SCHED_FIFO prio %d, pinned on CPU %d",
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
        log_msg("ERROR", "cannot create the inference thread: %s", strerror(rc));
        return -1;
    }
    d->worker_started = 1;

    log_msg("INFO", "inference thread started: SCHED_FIFO prio %d, CPU %d",
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
    log_msg("INFO", "generate request queued (%d chars)", (int) strlen(req->prompt));
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

    while (!g_stop) {
        memset(&msg, 0, sizeof(msg));
        rcvid = MsgReceive(d->chid, &msg, sizeof(msg), &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue; /* a signal: check g_stop */
            }
            log_msg("ERROR", "MsgReceive failed: %s", strerror(errno));
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

    rc = parse_args(argc, argv, &opt);
    if (rc == 2) {
        usage(argv[0]);
        return 0;
    }
    if (rc != 0) {
        usage(argv[0]);
        return 1;
    }
    if (install_handlers() != 0) {
        log_msg("FATAL", "cannot install the signal handlers: %s", strerror(errno));
        return 1;
    }

    memset(&d, 0, sizeof(d));
    d.opt = opt;
    pthread_mutex_init(&d.lock, NULL);
    pthread_cond_init(&d.cond, NULL);

    set_fifo(opt.prio);

    log_msg("INFO", "loading model '%s' (ctx=%d, threads=%d)", opt.model, opt.n_ctx, opt.n_threads);
    d.llm = cadence_llm_load(opt.model, opt.n_ctx, opt.n_threads, err, sizeof(err));
    if (!d.llm) {
        log_msg("FATAL", "%s", err);
        return 1;
    }
    log_msg("INFO", "model loaded");

    d.attach = name_attach(NULL, opt.name, 0);
    if (!d.attach) {
        log_msg("FATAL", "name_attach('%s') failed: %s", opt.name, strerror(errno));
        cadence_llm_free(d.llm);
        return 1;
    }
    d.chid = d.attach->chid;

    if (start_worker(&d) != 0) {
        name_detach(d.attach, 0);
        cadence_llm_free(d.llm);
        return 1;
    }

    log_msg("INFO", "cadenced ready on /dev/name/local/%s (pid %d)", opt.name, (int) getpid());
    dispatch_loop(&d);

    log_msg("INFO", "shutting down");
    g_stop = 1;
    pthread_mutex_lock(&d.lock);
    pthread_cond_broadcast(&d.cond);
    pthread_mutex_unlock(&d.lock);
    if (d.worker_started) {
        pthread_join(d.worker, NULL);
    }
    name_detach(d.attach, 0);
    cadence_llm_free(d.llm);
    log_msg("INFO", "bye");
    return 0;
}

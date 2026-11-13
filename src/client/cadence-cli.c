/*
 * cadence-cli — demo client for the Cadence QNX native LLM service.
 *
 * Connects to the daemon through the QNX name service, sends a prompt and
 * prints the tokens as they arrive, with the measured throughput
 * (tokens/s, ms/token, time to first token).
 */
#define _QNX_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/sched.h>
#include <time.h>
#include <unistd.h>

#include "cadence_msg.h"

#define CADENCE_DEFAULT_MAXTOK 128

/* Every message the service can send back. */
union cadence_rsp {
    cadence_rsp_accepted ack;
    cadence_rsp_token    tok;
    cadence_rsp_done     done;
    cadence_rsp_error    err;
    cadence_rsp_status   st;
};

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1000000.0;
}

static void usage(const char * prog)
{
    printf("Usage: %s [options]\n"
           "  -p, --prompt TEXT     prompt sent to the service (default: a French question)\n"
           "  -n, --name NAME       QNX name service entry (default %s)\n"
           "  -M, --max-tokens N    maximum number of tokens (default %d)\n"
           "  -s, --seed N          sampling seed\n"
           "      --ping            only check that the service answers\n"
           "      --status          print the real-time configuration of the service\n"
           "      --threads         print the 'pidin threads' table of the system\n"
           "  -h, --help            this help\n",
           prog, CADENCE_SERVICE_NAME, CADENCE_DEFAULT_MAXTOK);
}

static int send_ping(int coid)
{
    cadence_req_simple req;
    union cadence_rsp rsp;
    int rc;

    memset(&req, 0, sizeof(req));
    req.hdr.magic   = CADENCE_MAGIC;
    req.hdr.version = CADENCE_PROTO_VERSION;
    req.hdr.type    = CADENCE_REQ_PING;

    rc = MsgSend(coid, &req, sizeof(req), &rsp, sizeof(rsp));
    if (rc == -1) {
        fprintf(stderr, "cadence-cli: ping failed: %s\n", strerror(errno));
        return -1;
    }
    printf("cadence-cli: service answers (protocol v%u)\n", rsp.ack.hdr.version);
    return 0;
}

static int send_status(int coid)
{
    cadence_req_simple req;
    union cadence_rsp rsp;
    int rc;

    memset(&req, 0, sizeof(req));
    req.hdr.magic   = CADENCE_MAGIC;
    req.hdr.version = CADENCE_PROTO_VERSION;
    req.hdr.type    = CADENCE_REQ_STATUS;

    rc = MsgSend(coid, &req, sizeof(req), &rsp, sizeof(rsp));
    if (rc == -1) {
        fprintf(stderr, "cadence-cli: status failed: %s\n", strerror(errno));
        return -1;
    }
    printf("cadence-cli: service real-time configuration:\n");
    printf("    scheduling policy : %s\n", (rsp.st.policy == SCHED_FIFO) ? "SCHED_FIFO" : "other");
    printf("    priority          : %d\n", (int) rsp.st.priority);
    printf("    cpu affinity      : 0x%x\n", (unsigned) rsp.st.runmask);
    printf("    llama threads     : %d\n", (int) rsp.st.n_threads);
    printf("    context size      : %d tokens\n", (int) rsp.st.ctx_size);
    printf("    max tokens        : %d\n", (int) rsp.st.max_tokens);
    printf("    model             : %s\n", rsp.st.model);
    return 0;
}

static void print_threads(void)
{
    /* The QNX guest has no shell pipeline: pidin is called directly. */
    printf("cadence-cli: pidin threads is run by the in-guest script\n");
}

/* Connect to the service, waiting for it to show up (model loading). */
static int connect_service(const char * name, int wait_secs)
{
    int waited_ms = 0;

    for (;;) {
        int coid = name_open(name, 0);

        if (coid != -1) {
            return coid;
        }
        if (waited_ms >= wait_secs * 1000) {
            fprintf(stderr, "cadence-cli: cannot connect to '%s': %s\n", name, strerror(errno));
            return -1;
        }
        {
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = 200 * 1000 * 1000; /* 200 ms */
            nanosleep(&ts, NULL);
        }
        waited_ms += 200;
    }
}

/* Print the content of a file (the QNX guest has no cat). */
static int dump_file(const char * path)
{
    char buf[512];
    ssize_t n;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "cadence-cli: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t) n, stdout);
    }
    close(fd);
    return 0;
}

/* Receive the token stream and print it as it comes. */
static int stream_tokens(int chid, double * t_first, double * t_last,
                         int * n_tokens, double * server_ms)
{
    union cadence_rsp rsp;
    rcvid_t rcvid;

    *t_first = 0.0;
    *t_last  = 0.0;
    *n_tokens = 0;
    *server_ms = 0.0;

    for (;;) {
        double t_now = now_ms();

        memset(&rsp, 0, sizeof(rsp));
        rcvid = MsgReceive(chid, &rsp, sizeof(rsp), NULL);
        if (rcvid == -1) {
            fprintf(stderr, "cadence-cli: MsgReceive failed: %s\n", strerror(errno));
            return -1;
        }
        if (rcvid == 0) {
            continue; /* pulse */
        }
        if (rsp.tok.hdr.magic != CADENCE_MAGIC) {
            fprintf(stderr, "cadence-cli: bad magic on the stream\n");
            return -1;
        }
        if (rsp.tok.hdr.type == CADENCE_RSP_TOKEN) {
            if (*t_first == 0.0) {
                *t_first = t_now;
            }
            *t_last = t_now;
            (*n_tokens)++;
            fputs(rsp.tok.text, stdout);
            fflush(stdout);
            continue;
        }
        if (rsp.done.hdr.type == CADENCE_RSP_DONE) {
            *server_ms = rsp.done.total_ms;
            printf("\n");
            return 0;
        }
        if (rsp.err.hdr.type == CADENCE_RSP_ERROR) {
            fprintf(stderr, "cadence-cli: service error: %s\n", rsp.err.message);
            return -1;
        }
        fprintf(stderr, "cadence-cli: unexpected message type %u\n", rsp.done.hdr.type);
        return -1;
    }
}

int main(int argc, char ** argv)
{
    const char * prompt = "Quelle est la capitale de la France ? Réponds en français, en une seule phrase.";
    const char * name = CADENCE_SERVICE_NAME;
    int max_tokens = CADENCE_DEFAULT_MAXTOK;
    int do_ping = 0, do_status = 0, do_threads = 0;
    int wait_secs = 0;
    const char * cat_path = NULL;
    uint32_t seed = 0;
    cadence_req_generate req;
    union cadence_rsp ack;
    int coid, chid, i, rc;
    double t0, t_first = 0.0, t_last = 0.0, server_ms = 0.0;
    int n_tokens = 0;

    for (i = 1; i < argc; i++) {
        const char * a = argv[i];
        const char * v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(a, "--ping")) {
            do_ping = 1;
            continue;
        }
        if (!strcmp(a, "--status")) {
            do_status = 1;
            continue;
        }
        if (!strcmp(a, "--threads")) {
            do_threads = 1;
            continue;
        }
        if (!strcmp(a, "--cat")) {
            if (!v) {
                fprintf(stderr, "cadence-cli: missing file name\n");
                return 1;
            }
            cat_path = v;
            i++;
            continue;
        }
        if (!strcmp(a, "--wait")) {
            if (!v) {
                fprintf(stderr, "cadence-cli: missing number of seconds\n");
                return 1;
            }
            wait_secs = atoi(v);
            i++;
            continue;
        }
        if (!v) {
            fprintf(stderr, "cadence-cli: missing value for option '%s'\n", a);
            return 1;
        }
        if (!strcmp(a, "-p") || !strcmp(a, "--prompt")) {
            prompt = v;
        } else if (!strcmp(a, "-n") || !strcmp(a, "--name")) {
            name = v;
        } else if (!strcmp(a, "-M") || !strcmp(a, "--max-tokens")) {
            max_tokens = atoi(v);
        } else if (!strcmp(a, "-s") || !strcmp(a, "--seed")) {
            seed = (uint32_t) strtoul(v, NULL, 0);
        } else {
            fprintf(stderr, "cadence-cli: unknown option '%s'\n", a);
            return 1;
        }
        i++;
    }

    if (cat_path) {
        rc = dump_file(cat_path);
        return (rc == 0) ? 0 : 1;
    }

    coid = connect_service(name, wait_secs);
    if (coid == -1) {
        fprintf(stderr, "cadence-cli: is cadenced running?\n");
        return 1;
    }

    if (do_ping) {
        rc = send_ping(coid);
        name_close(coid);
        return (rc == 0) ? 0 : 1;
    }
    if (do_status) {
        rc = send_status(coid);
        name_close(coid);
        return (rc == 0) ? 0 : 1;
    }
    if (do_threads) {
        print_threads();
        name_close(coid);
        return 0;
    }

    chid = ChannelCreate(0);
    if (chid == -1) {
        fprintf(stderr, "cadence-cli: ChannelCreate failed: %s\n", strerror(errno));
        name_close(coid);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.hdr.magic   = CADENCE_MAGIC;
    req.hdr.version = CADENCE_PROTO_VERSION;
    req.hdr.type    = CADENCE_REQ_GENERATE;
    req.max_tokens  = max_tokens;
    req.client_pid  = (int) getpid();
    req.client_chid = chid;
    req.seed        = seed;
    snprintf(req.prompt, sizeof(req.prompt), "%s", prompt);

    printf("cadence-cli: prompt sent to '%s': %s\n", name, prompt);
    fflush(stdout);

    t0 = now_ms();
    rc = MsgSend(coid, &req, sizeof(req), &ack, sizeof(ack));
    if (rc == -1) {
        fprintf(stderr, "cadence-cli: the service refused the request: %s\n", strerror(errno));
        ChannelDestroy(chid);
        name_close(coid);
        return 1;
    }
    if (ack.ack.hdr.type == CADENCE_RSP_ERROR) {
        fprintf(stderr, "cadence-cli: service error: %s\n", ack.err.message);
        ChannelDestroy(chid);
        name_close(coid);
        return 1;
    }

    rc = stream_tokens(chid, &t_first, &t_last, &n_tokens, &server_ms);

    printf("cadence-cli: --- measurements ---\n");
    printf("    tokens             : %d\n", n_tokens);
    if (t_first > 0.0) {
        printf("    time to first token : %.0f ms\n", t_first - t0);
        printf("    client total time  : %.0f ms\n", t_last - t0);
        if (n_tokens > 1) {
            printf("    ms/token (client)  : %.1f\n", (t_last - t_first) / (n_tokens - 1));
            printf("    tokens/s (client)  : %.2f\n",
                   (n_tokens - 1) * 1000.0 / (t_last - t_first));
        }
    }
    if (server_ms > 0.0 && n_tokens > 0) {
        printf("    ms/token (server)  : %.1f\n", server_ms / n_tokens);
        printf("    tokens/s (server)  : %.2f\n", n_tokens * 1000.0 / server_ms);
    }

    ChannelDestroy(chid);
    name_close(coid);
    return (rc == 0) ? 0 : 1;
}

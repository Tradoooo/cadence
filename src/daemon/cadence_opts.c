/*
 * cadence_opts.c — command line options of the Cadence daemon.
 */
#define _QNX_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cadence_opts.h"

void cadence_usage(const char * prog)
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

int cadence_parse_args(int argc, char ** argv, cadence_opts_t * opt)
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

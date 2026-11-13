/*
 * cadence_opts.h — command line options of the Cadence QNX service.
 */
#ifndef CADENCE_OPTS_H
#define CADENCE_OPTS_H

#include <stdint.h>

#define CADENCE_DEFAULT_NAME    "cadence-llm"
#define CADENCE_DEFAULT_MODEL   "/proc/boot/model.gguf"
#define CADENCE_DEFAULT_CTX     4096
#define CADENCE_DEFAULT_THREADS 4
#define CADENCE_DEFAULT_PRIO    20
#define CADENCE_DEFAULT_MAXTOK  128

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

/* Print the usage of the program. */
void cadence_usage(const char * prog);

/*
 * Parse the command line: 0 on success, 1 on error, 2 when --help was asked.
 */
int cadence_parse_args(int argc, char ** argv, cadence_opts_t * opt);

#endif /* CADENCE_OPTS_H */

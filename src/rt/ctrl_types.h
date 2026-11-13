/*
 * ctrl_types.h -- types shared by the control task and its JSON reporter
 * (QNX 8.0).  Kept in its own header so that the reporting code stays a
 * separate compilation unit.
 */
#ifndef CTRL_TYPES_H
#define CTRL_TYPES_H

#include <signal.h>
#include <stdint.h>
#include <sys/neutrino.h>

#define RT_WORST_KEEP     20
/* QNX kernel tick: 100 us so that a 1 kHz loop is not quantised to 1 ms. */
#define RT_KERNEL_TICK_NS 100000u

struct worst_event {
    uint64_t t_ns;       /* timestamp of the wake-up, relative to start */
    uint64_t jitter_ns;  /* wake-up jitter (late wake-up) */
    uint64_t skipped;    /* number of periods skipped */
    uint64_t cycle;      /* cycle index */
};

struct rt_config {
    uint64_t period_ns;
    double   duration_s;
    int      priority;
    int      cpu;        /* -1: no affinity */
    const char *tag;
    const char *out_path;
};

struct rt_state {
    struct rt_config cfg;

    uint64_t cycles;
    uint64_t deadline_misses;
    uint64_t miss_events;
    int      in_miss;
    uint64_t skipped_periods;
    uint64_t start_ns;

    uint64_t *samples;
    size_t    nsamples;
    size_t    cap;

    struct worst_event worst[RT_WORST_KEEP];
    size_t            nworst;

    uint64_t work_sum_ns;
    uint64_t work_max_ns;
    uint64_t work_min_ns;

    int      affinity_ok;
    int      affinity_errno;
    int      prio_ok;
    int      cp_changed;
    int      cp_errno;
    int      hi_res;
    int      no_memory;
    struct _clockperiod old_cp;
};

#endif /* CTRL_TYPES_H */

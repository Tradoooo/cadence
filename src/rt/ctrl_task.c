/*
 * ctrl_task.c -- Cadence real-time proof: hard real-time control task.
 *
 * Runs a fixed-period control loop (default 1 kHz) with clock_gettime()
 * (CLOCK_MONOTONIC) and measures its OWN wake-up jitter: the gap between
 * the instant the thread was supposed to wake up and the instant it
 * actually woke up.
 *
 * The task is scheduled SCHED_FIFO (priority 63 by default) and pinned to a
 * single CPU with ThreadCtl(_NTO_TCTL_RUNMASK) so that the LLM inference
 * (running on the other cores) cannot perturb the control deadline.
 *
 * Statistics (min / mean / max / p99, deadline misses, 20 worst events) are
 * exported as JSON on stdout and optionally to a file (e.g. /dev/shmem/...).
 *
 * QNX 8.0 / x86_64.  Build: qcc -o ctrl_task ctrl_task.c
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include "rt_common.h"
#include "ctrl_types.h"

#define RT_SAMPLES_INIT   65536u
#define RT_SAMPLES_MAX    8000000u

void ctrl_emit_report(struct rt_state *st, const rt_stats *jst, rt_json *j);

static void apply_clock_period(struct rt_state *st)
{
    struct _clockperiod cp;

    cp.nsec  = RT_KERNEL_TICK_NS;
    cp.fract = 0;
    if (ClockPeriod(CLOCK_MONOTONIC, &cp, &st->old_cp, 0) == 0) {
        st->cp_changed = 1;
    } else {
        st->cp_errno = errno;
    }
}

/* ------------------------------------------------------------- helpers */

static volatile sig_atomic_t g_stop = 0;

static void stop_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int samples_push(struct rt_state *st, uint64_t v)
{
    if (st->nsamples >= st->cap) {
        size_t    ncap = st->cap ? st->cap * 2 : RT_SAMPLES_INIT;
        uint64_t *nb;

        if (ncap > RT_SAMPLES_MAX) {
            return -1;
        }
        nb = realloc(st->samples, ncap * sizeof(uint64_t));
        if (nb == NULL) {
            return -1;
        }
        st->samples = nb;
        st->cap     = ncap;
    }
    st->samples[st->nsamples++] = v;
    return 0;
}

/* Keeps the RT_WORST_KEEP largest jitters, sorted descending. */
static void worst_insert(struct rt_state *st, const struct worst_event *ev)
{
    size_t i, pos;

    if (st->nworst < RT_WORST_KEEP) {
        pos = st->nworst++;
    } else if (ev->jitter_ns <= st->worst[RT_WORST_KEEP - 1].jitter_ns) {
        return;
    } else {
        pos = RT_WORST_KEEP - 1;
    }
    for (i = pos; i > 0 && st->worst[i - 1].jitter_ns < ev->jitter_ns; i--) {
        st->worst[i] = st->worst[i - 1];
    }
    st->worst[i] = *ev;
}

/* --------------------------------------------------- scheduling setup */

static int apply_priority(struct rt_state *st)
{
    struct sched_param sp;
    int                rc;

    sp.sched_priority = st->cfg.priority;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != 0) {
        return -1;
    }
    st->prio_ok = 1;
    return 0;
}

/* QNX: the runmask is a uint64_t value cast to void*, not a pointer. */
static int apply_affinity(struct rt_state *st)
{
    uint64_t runmask;

    if (st->cfg.cpu < 0) {
        st->affinity_ok = 1;   /* explicitly disabled */
        return 0;
    }
    runmask = (uint64_t)1 << (unsigned)st->cfg.cpu;
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask) != 0) {
        st->affinity_errno = errno;
        return -1;
    }
    st->affinity_ok = 1;
    return 0;
}

/* ------------------------------------------------------- control step */

/* Tiny simulated plant: a PI controller, so the loop does real work. */
struct plant {
    double setpoint;
    double state;
    double integral;
    double last_u;
};

static void control_step(struct plant *p)
{
    double err = p->setpoint - p->state;
    double u;

    p->integral += err;
    u = 0.5 * err + 0.05 * p->integral;
    if (u > 100.0) {
        u = 100.0;
    } else if (u < -100.0) {
        u = -100.0;
    }
    p->state   += 0.01 * (u - p->state);
    p->last_u   = u;
}

/* ----------------------------------------------------------- rt loop */

/* High-resolution wait: TimerTimeout() on CLOCK_MONOTONIC (ns granularity,
 * needs PROCMGR_AID_HIGH_RESOLUTION_TIMER, i.e. root) with a fallback on
 * clock_nanosleep() if the kernel call is refused.  The deadline chain is
 * kept by the caller, so drift is never absorbed into the jitter. */
static void rt_wait_until(struct rt_state *st, uint64_t deadline)
{
    uint64_t now = rt_now_ns();

    while (now < deadline) {
        uint64_t delta = deadline - now;

        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_NANOSLEEP, NULL, &delta, NULL) != 0) {
            struct timespec ts;

            rt_ns_to_ts(deadline, &ts);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            return;
        }
        st->hi_res = 1;
        now = rt_now_ns();
    }
}

/* Records one cycle: jitter, deadline miss, work time, worst events, and
 * re-anchors the deadline chain when periods were lost. */
static void record_cycle(struct rt_state *st, uint64_t wake,
                         uint64_t work, uint64_t *next_deadline)
{
    uint64_t jitter  = (wake > *next_deadline) ? (wake - *next_deadline) : 0;
    uint64_t skipped = jitter / st->cfg.period_ns;

    if (work < st->work_min_ns) {
        st->work_min_ns = work;
    }
    if (work > st->work_max_ns) {
        st->work_max_ns = work;
    }
    st->work_sum_ns += work;

    if (jitter > st->cfg.period_ns) {
        st->deadline_misses++;
        st->skipped_periods += skipped;
        if (!st->in_miss) {
            st->miss_events++;
            st->in_miss = 1;
        }
        /* Re-anchor the deadline chain: the periods lost during the stall are
         * skipped instead of being charged to the following cycles (otherwise
         * one stall looks like N consecutive misses). */
        *next_deadline += skipped * st->cfg.period_ns;
    } else {
        st->in_miss = 0;
    }
    if (samples_push(st, jitter) != 0) {
        st->no_memory = 1;
    }
    if (skipped > 0) {
        struct worst_event ev;

        ev.t_ns      = wake - st->start_ns;
        ev.jitter_ns = jitter;
        ev.skipped   = skipped;
        ev.cycle     = st->cycles;
        worst_insert(st, &ev);
    }
    st->cycles++;
}

static void *rt_thread(void *arg)
{
    struct rt_state *st = arg;
    struct plant      pl = { 1.0, 0.0, 0.0, 0.0 };
    uint64_t          deadline;
    uint64_t          next_deadline;
    uint64_t          wake;
    uint64_t          work_end;

    apply_priority(st);
    apply_affinity(st);
    apply_clock_period(st);

    st->work_min_ns = UINT64_MAX;
    deadline = rt_now_ns();
    st->start_ns = deadline;
    next_deadline = deadline;

    while (!g_stop) {
        control_step(&pl);
        work_end = rt_now_ns();

        next_deadline += st->cfg.period_ns;
        rt_wait_until(st, next_deadline);
        wake = rt_now_ns();

        {
            uint64_t work = work_end - deadline;

            record_cycle(st, wake, work, &next_deadline);
        }
        deadline = wake;

        if (st->no_memory) {
            break;
        }

        if (st->cfg.duration_s > 0.0 &&
            (double)(wake - st->start_ns) / 1e9 >= st->cfg.duration_s) {
            break;
        }
    }
    return NULL;
}

/* --------------------------------------------------------- reporting */

/* ------------------------------------------------------------- main */

static void usage(const char *p)
{
    printf("usage: %s [-p period_us] [-d duration_s] [-P priority] [-c cpu]\n"
           "          [-t tag] [-o out.json]\n"
           "  -p period_us   loop period in microseconds (default 1000 = 1 kHz)\n"
           "  -d duration_s  run duration (default 0: until SIGTERM)\n"
           "  -P priority    SCHED_FIFO priority (default 63)\n"
           "  -c cpu         CPU to pin the task to (-1 = no pinning)\n"
           "  -t tag         label written in the JSON report\n"
           "  -o path        write the JSON report to path as well as stdout\n",
           p);
}

static int parse_args(int argc, char **argv, struct rt_config *cfg)
{
    int i;

    cfg->period_ns  = 1000000ULL;
    cfg->duration_s = 0.0;
    cfg->priority   = 63;
    cfg->cpu        = 0;
    cfg->tag        = "run";
    cfg->out_path   = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cfg->period_ns = strtoull(argv[++i], NULL, 10) * 1000ULL;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            cfg->duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            cfg->priority = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg->cpu = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg->tag = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            cfg->out_path = argv[++i];
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    if (cfg->period_ns == 0) {
        fprintf(stderr, "%s: period must be > 0\n", argv[0]);
        return -1;
    }
    return 0;
}

/* Starts the control thread with SCHED_FIFO at the configured priority. */
static int spawn_control_thread(struct rt_state *st, pthread_t *tid)
{
    pthread_attr_t     attr;
    struct sched_param sp;
    int                rc;

    sp.sched_priority = st->cfg.priority;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);
    rc = pthread_create(tid, &attr, rt_thread, st);
    pthread_attr_destroy(&attr);
    return rc;
}

static void install_stop_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

int main(int argc, char **argv)
{
    struct rt_state  st;
    rt_stats          stats;
    pthread_t        tid;
    rt_json          json;

    memset(&st, 0, sizeof(st));
    if (parse_args(argc, argv, &st.cfg) != 0) {
        return 2;
    }
    install_stop_handlers();

    st.samples = malloc(RT_SAMPLES_INIT * sizeof(uint64_t));
    if (st.samples == NULL) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        return 1;
    }
    st.cap = RT_SAMPLES_INIT;

    if (spawn_control_thread(&st, &tid) != 0) {
        fprintf(stderr, "%s: pthread_create failed\n", argv[0]);
        return 1;
    }
    pthread_join(tid, NULL);

    if (st.cp_changed) {
        ClockPeriod(CLOCK_MONOTONIC, &st.old_cp, NULL, 0);
    }
    rt_compute_stats(&stats, st.samples, st.nsamples);

    if (rt_json_init(&json, 8192) != 0) {
        return 1;
    }
    ctrl_emit_report(&st, &stats, &json);
    printf("%s\n", json.buf);
    fflush(stdout);

    if (st.cfg.out_path != NULL) {
        FILE *f = fopen(st.cfg.out_path, "w");

        if (f != NULL) {
            fprintf(f, "%s\n", json.buf);
            fclose(f);
        } else {
            fprintf(stderr, "%s: cannot write %s: %s\n",
                    argv[0], st.cfg.out_path, strerror(errno));
        }
    }
    rt_json_free(&json);
    free(st.samples);
    return st.deadline_misses == 0 ? 0 : 3;
}

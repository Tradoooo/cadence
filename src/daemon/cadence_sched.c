/*
 * cadence_sched.c — the QNX-specific scheduling and memory-placement layer.
 */
#define _QNX_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/sched.h>
#include <unistd.h>

#include "cadence_sched.h"
#include "cadence_util.h"

/* Set from the signal handler, read by the dispatch loop. */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void) sig;
    g_stop = 1;
}

int cadence_install_handlers(void)
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

void cadence_request_stop(void)
{
    g_stop = 1;
}

int cadence_stop_requested(void)
{
    return g_stop;
}

int cadence_set_thread_prio(int prio)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        cadence_log("WARN", "cannot set SCHED_FIFO prio %d on this thread: %s",
                    prio, strerror(errno));
        return -1;
    }
    return 0;
}

int cadence_pin_thread(int cpu)
{
    uint64_t mask = (uint64_t) 1 << cpu;

    /* ThreadCtl applies to the calling thread: pin ourselves. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, &mask) == -1) {
        cadence_log("WARN", "cannot pin the thread on CPU %d: %s",
                    cpu, strerror(errno));
        return -1;
    }
    return 0;
}

int cadence_create_worker(pthread_t * thread, int prio,
                          void * (*start)(void *), void * arg)
{
    pthread_attr_t attr;
    struct sched_param sp;
    int rc;

    pthread_attr_init(&attr);
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    /*
     * EXPLICIT_SCHED is the important part: with the default (inherit), the
     * inference thread would inherit the priority of whichever thread
     * created it, and a high-priority client could raise it by accident.
     */
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);

    rc = pthread_create(thread, &attr, start, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        cadence_log("ERROR", "cannot create the inference thread: %s", strerror(rc));
        return -1;
    }
    return 0;
}

int cadence_lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        cadence_log("WARN", "mlockall failed: %s (the model may be swapped out)",
                    strerror(errno));
        return -1;
    }
    return 0;
}

long cadence_prefault_file(const char * path)
{
    char buf[65536];
    long total = 0;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return -1;
    }
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        total += (long) n;
    }
    close(fd);
    return total;
}

/*
 * cadence_sched.h — the QNX-specific scheduling and memory-placement layer.
 *
 * Everything that makes Cadence a real-time service rather than a Linux port
 * lives here:
 *
 *   - the stop flag and the signal handlers (the dispatch loop must leave on
 *     EINTR, so SA_RESTART must stay off);
 *   - SCHED_FIFO priority on the calling thread;
 *   - CPU pinning with ThreadCtl(_NTO_TCTL_RUNMASK);
 *   - the creation of the inference thread with an EXPLICIT scheduling policy
 *     (PTHREAD_EXPLICIT_SCHED), so that no priority can ever be inherited
 *     into it;
 *   - mlockall() and file pre-faulting, so that the inference path never
 *     takes a page fault nor a swap hit.
 *
 * Message priority inheritance: QNX boosts the priority of a thread blocked
 * in MsgReceive to the priority of the sender. That is why the heavy work
 * never happens in the dispatch thread: the dispatch thread only relays a
 * request, and the inference thread runs with a fixed, explicit priority that
 * no sender can raise.
 */
#ifndef CADENCE_SCHED_H
#define CADENCE_SCHED_H

#include <pthread.h>
#include <sys/types.h>

/* Install SIGTERM/SIGINT handlers and ignore SIGPIPE. Returns 0 on success. */
int cadence_install_handlers(void);

/* Ask for a graceful stop (also called from the signal handler). */
void cadence_request_stop(void);

/* Non-zero once a stop has been requested. */
int cadence_stop_requested(void);

/* Set the calling thread to SCHED_FIFO with the given priority. */
int cadence_set_thread_prio(int prio);

/* Pin the calling thread on a single CPU. Returns 0 on success. */
int cadence_pin_thread(int cpu);

/* Create a thread with an explicit SCHED_FIFO policy and priority. */
int cadence_create_worker(pthread_t * thread, int prio,
                           void * (*start)(void *), void * arg);

/*
 * Lock the whole address space in RAM. Called once, before the model is
 * loaded, so that no page of the model or of the KV cache can be swapped
 * out during a generation.
 */
int cadence_lock_memory(void);

/*
 * Read a file sequentially to force the page cache to hold it, so that the
 * first generation does not pay the fault cost. Returns the number of bytes
 * read, or -1 on error.
 */
long cadence_prefault_file(const char * path);

#endif /* CADENCE_SCHED_H */

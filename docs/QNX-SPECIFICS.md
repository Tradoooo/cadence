# QNX specifics — what we use, and why it is QNX

> Every API named here was checked against the **QNX SDP 8.0** installed on the
> build machine (`~/qnx800`). Where the documentation and the shipped binaries
> disagree, we say so.

---

## 1. name_attach and MsgSend vs sockets

### What we do

The service publishes itself in the microkernel's pathname space:

```c
#include <sys/dispatch.h>     /* name_attach, name_open, name_detach */

/* server side */
name_attach_t *na = name_attach(NULL, "/dev/name/local/cadence", 0);
/* na->chid  → the channel to MsgReceive() on
   na->mntid → the mount id of the name  */

/* client side */
int coid = name_open("/dev/name/local/cadence", 0);
/* coid is a connection id: MsgSend(coid, …) / MsgReply() */
```

### Verified facts about QNX 8.0

- The declaration lives in **`<sys/dispatch.h>`**, not in a separate `<name.h>`
  (there is no `<name.h>` in the 8.0 target headers — confirmed by
  `find $QNX_TARGET -name name.h`, which returns nothing).
- The symbols are in **libc**: `nm -D $QNX_TARGET/x86_64/lib/libc.so | grep
  name_` lists `name_attach`, `name_detach`, `name_open`.
- The name is registered **under `/dev/name/local/`**. The SDP 8.0
  documentation is explicit: *"The `name_attach()` function puts the name path
  into the pathname space under `/dev/name/local/`"*, and *"This pathname
  space is unprivileged, meaning your process doesn't need the
  `PROCMGR_AID_PATHSPACE` ability"*.
- **A public channel needs an ability.** From the SDP 8.0 `name_attach()`
  documentation: *"In order to create a public channel (i.e., without
  `_NTO_CHF_PRIVATE` set), your process needs the `PROCMGR_AID_PUBLIC_CHANNEL`
  ability enabled."* `PROCMGR_AID_PUBLIC_CHANNEL` is defined as `44u` in
  `<sys/procmgr.h>`, and — importantly — it is
  `ABLE_ALLOW_ROOT|ABLE_ALLOW_NONROOT|ABLE_NO_SUBRANGE`, so a non-root process
  can hold it.
- If you pass `NULL` as the `dispatch_t *`, `name_attach()` creates the
  dispatch structure and the channel for you, and the channel is created with
  `_NTO_CHF_COID_DISCONNECT`, `_NTO_CHF_UNBLOCK` and `_NTO_CHF_DISCONNECT`
  set. That means **the server must handle the disconnect pulses** or it will
  not notice clients going away.
- The SDP documentation warns: *"There's no guarantee that the client will end
  up talking to the server it thinks it's connecting to"*. If that matters,
  use `resmgr_attach()` instead of `name_attach()` (both are in libc — the
  latter is the full resource-manager flavour).

### The comparison we are making

| | QNX message passing (what we do) | A localhost TCP/HTTP socket (what we don't) |
|---|---|---|
| Path in the hot path | `MsgSend()` → one kernel copy + direct thread wakeup | `write()` → TCP state machine → socket buffers → peer wakeup |
| Configuration | none — a name in the pathname space | port, address family, buffer sizes, `SO_REUSEADDR`, firewall rules |
| Isolation | the kernel knows exactly which client connects to which channel | an address and a port; `SO_PEERCRED`-style tricks needed for identity |
| Failure | channel severed → the kernel unblocks the peer with an error | half-open sockets, `SIGPIPE`, TIME_WAIT, orphaned connections |
| Idiomatic | yes — resource managers work exactly this way | a portable workaround on an RTOS |
| Cost | ~microseconds | ~tens of microseconds, plus periodic work in the network stack |

At 40 ms per token, the transport is not the bottleneck. The argument is not
about speed — it is about **adding as little as possible to a system whose
whole promise is that nothing else is disturbed**. A socket stack in the path
of a control-critical system is a component that can be scheduled, can buffer,
can be delayed, and can fail in ways a kernel channel cannot.

**Note for the "Best use of QNX-specific Technologies" category:** this is the
mechanism QNX uses for *its own* resource managers. Using it for an LLM
service is the point.

---

## 2. `SCHED_FIFO` and `pthread_setschedparam()`

### The API

```c
#include <sched.h>
#include <pthread.h>

struct sched_param sp = { .sched_priority = 10 };
pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
```

### Verified facts about QNX 8.0

- `SCHED_FIFO` is `1` in `<sched.h>` (`SCHED_RR` is `2`, `SCHED_OTHER` is `3`,
  `SCHED_SPORADIC` is `4`).
- `pthread_setschedparam()` and `pthread_getschedparam()` are declared in
  `<pthread.h>`.
- **256 priority levels exist.** From the SDP 8.0 System Architecture guide:
  *"The OS supports a total of 256 scheduling priority levels. An unprivileged
  thread can set its priority to a level from 1 to 63 (the highest
  unprivileged priority), independent of the scheduling policy. Only threads
  with the `PROCMGR_AID_PRIORITY` ability enabled are allowed to set
  priorities above 63."*
- **Priority 255 is reserved for interprocessor interrupts**, and **254 is by
  default reserved for the clock Interrupt Service Thread** (SDP 8.0 migration
  guide). So the usable ceiling for a user process is 253, and the floor is 1.
- The idle thread has priority 0 and is always ready.
- `FIFO scheduling`: a thread *"continues executing until it voluntarily
  relinquishes control (e.g., it blocks) or is preempted by a higher-priority
  thread"*. **No time slice.** That is exactly what a background worker wants:
  it must not be handed a timeslice that competes with the control task for
  the core.
- On a multicore system, *"the scheduler runs the highest-priority READY
  thread on one of the available processors"*. Priority is therefore a
  **global** property, and affinity (§3) is what localises it.

### How we use it

```
priority 63+   cadence-ctrl   (control task — needs PROCMGR_AID_PRIORITY)
priority ~20   cadence server thread
priority ~10   cadence inference thread
```

The inference thread sits **below** the control task on purpose. When the
control task becomes ready, the kernel preempts the inference thread
immediately — this is the guarantee that the generation cannot delay the
control loop.

`TODO(QNX):` the exact priorities used in the shipped build, and the measured
preemption latency.

### The alternative we rejected

`SCHED_RR` would give the inference thread a timeslice. That sounds
politer, but a timeslice is a *periodic, scheduler-visible* disturbance: the
inference thread would be preempted on a timer whether or not anything
important was ready, and the control task could land behind it. `SCHED_FIFO`
at a low priority gives the control task the CPU the moment it asks.

---

## 3. Processor affinity: `ThreadCtl(_NTO_TCTL_RUNMASK)`

### The API

```c
#include <sys/neutrino.h>
#include <string.h>

/* the number of processors comes from the system page */
uint64_t mask = 0;
RMSK_SET(2, &mask);      /* allow CPU 2 */
RMSK_SET(3, &mask);      /* allow CPU 3 */

if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)&mask) == -1) {
    perror("ThreadCtl(_NTO_TCTL_RUNMASK)");
}
```

### Verified facts about QNX 8.0

- `_NTO_TCTL_RUNMASK` is `4` in `<sys/neutrino.h>`. The neighbouring commands
  are `_NTO_TCTL_RUNMASK_GET_AND_SET` (`6`),
  `_NTO_TCTL_RUNMASK_GET_AND_SET_64` (`7`) and
  `_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT` (`10`).
- **The runmask must match a cluster.** From the SDP 8.0 Programmer's Guide
  (Processor affinity, clusters, runmasks): *"If this runmask matches a
  cluster, the call succeeds; otherwise it fails with `EINVAL`."*
- **Clusters are defined at boot**, by the startup program. The SDP 8.0
  `startup-x86` documentation for `-c`: *"Specify cluster definitions, where a
  cluster is a group of associated processors."* Syntax:
  `-c name:mask[,name:mask...]`, e.g. `-c llm:0xf0,ctrl:0x0f` for a 4-core
  system. Reserved names begin with an underscore: `_all` (every processor)
  and `_cpu-<num>` (one per processor).
- **Restrictions** (SDP 8.0): no processor may be in more than eight
  clusters; each thread is associated with only one cluster; a thread can run
  only on processors belonging to the cluster indicated by its runmask.
- **`ThreadCtl()` always targets the calling thread.** To change another
  thread, use `ThreadCtlExt(pid, tid, cmd, data)`.
- **SDP 8.0 change:** *"Updating the runmask or inherit mask is permitted only
  on the active thread."* (SDP 8.0 migration guide.) This is a real migration
  hazard for code written for 7.x that set a runmask from another thread.
- **Inherit mask.** A thread's runmask at start-up comes from its creator's
  *inherit* mask, not its runmask. To set both, use
  `_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT` with `struct _thread_runmask`
  (`int size;` followed by `unsigned runmask[size];` and
  `unsigned inherit_mask[size];`). `RMSK_SIZE(num_cpu)`,
  `RMSK_SET(cpu, p)`, `RMSK_CLR(cpu, p)` and `RMSK_ISSET(cpu, p)` are the
  helpers, defined in `<sys/neutrino.h>`.
- **Query the clusters:** `pidin syspage=cluster` prints each cluster's name
  and runmask.
- **Client runmask inheritance.** When a thread receives a *message*, it
  inherits the sender's runmask (unless the channel was created with
  `_NTO_CHF_INHERIT_RUNMASK`… precisely: the *sender's* runmask replaces the
  receiver's). This matters for us: **the server thread inherits the runmask
  of whoever first sent it a message**, so the server thread's affinity is
  not under our control after the first client connects. This is one reason
  the inference work happens on a **separate thread with an explicitly set
  runmask** rather than on the receiving thread.

### How we use it

At boot, `startup-x86` defines the clusters:

```
startup-x86 ... -c llm:0xf0,ctrl:0x0f
```

Then:

- the **control task** sets its runmask to the `ctrl` cluster;
- the **inference thread** sets its runmask to the `llm` cluster;
- the two clusters are disjoint, so the inference thread *cannot* migrate onto
  the control task's core.

Because the clusters are disjoint, `ThreadCtl()` succeeds; a raw bitmask that
did not correspond to a boot-time cluster would fail with `EINVAL`.

`TODO(QNX):` the exact cluster names and masks in the shipped image, and the
`pidin syspage=cluster` output as evidence.

---

## 4. Adaptive partitioning (APS) in QNX 8.0

### What APS was

QNX's **Adaptive Partitioning Scheduler** gave safety-certified systems
per-partition CPU budgets, a guaranteed minimum allocation, and a *bankruptcy
event* when a partition exceeded its budget. It was selected through the
scheduler-extension mechanism (`SCHED_EXT_APS`), driven through
`sched_query`/`sched_ctl` with `SCHED_EXT_APS_CMD_BASE` (`200`), and
configured at boot through the `script_extsched_aps` structure in
`<sys/image.h>` (parent partition, budget, critical band, partition id, name).

### What actually happens in QNX 8.0

**APS is not available in QNX 8.0.** The `procnto-smp-instr` shipped in the
8.0 SDP contains the literal string:

```
$ strings $QNX_TARGET/x86_64/boot/sys/procnto-smp-instr | grep 'APS is no longer supported'
APS is no longer supported
```

The headers still carry the vestiges — `<sched.h>` defines `SCHED_EXT_APS`
(`1`) and `SCHED_EXT_APS_CMD_BASE` (`200`), and `<sys/image.h>` still defines
`struct script_extsched_aps` — but the kernel refuses the feature. There is
also no `aps` binary anywhere in the 8.0 target tree.

This is worth stating plainly, because it is the kind of detail that
separates a project that has actually built on QNX 8.0 from one that has
copied a QNX 7.x slide deck. It is also **the** place where the honest
answer is: *we cannot use APS on 8.0, so we prove the same property with the
mechanisms 8.0 does have.*

### What we do instead

The property we want is: **a critical control task is never perturbed by
generation.** QNX 8.0 gives us three ways to obtain it, and we use all three:

| Mechanism | What it buys | Where |
|---|---|---|
| **Priority separation** (`SCHED_FIFO`, control at 63+, inference at ~10) | The control task preempts the inference thread the instant it becomes ready. | [§2](#2-sched_fifo-and-pthread_setschedparam) |
| **Affinity separation** (`_NTO_TCTL_RUNMASK` over disjoint clusters) | The inference thread cannot migrate onto the control core. | [§3](#3-processor-affinity-threadctl_nto_tctl_runmask) |
| **Kernel trace evidence** (`tracelogger` → `traceprinter`) | The claim is corroborated by the kernel's own record of what ran, where, and at which priority. | [§5](#5-tracelogger-and-traceprinter) |

Together these give the *observable* consequence of APS on this workload: the
control task's deadline behaviour under generation is indistinguishable from
its behaviour at idle.

**We do not claim APS.** We claim a measured isolation property, and we show
the measurement.

`TODO(QNX):` the measured deadline comparison (idle vs. generating) and the
raw trace.

---

## 5. `tracelogger` and `traceprinter`

### What they are

`tracelogger` is a client of the **instrumented kernel**. The QNX 8.0 target
ships only instrumented kernels (`procnto-smp-instr`), so tracing is always
available. `tracelogger` collects kernel events — thread scheduling,
interrupt handling, IPC, process/thread lifecycle — into a `.kev` file
("kernel events") which `traceprinter` can filter and format.

### Verified facts about QNX 8.0

- Location: `usr/sbin/tracelogger` and `usr/bin/traceprinter` in the target
  tree. **`tracelogger` must run as root.**
- Default output file: `/dev/shmem/tracebuffer.kev`. (This matters: on a guest
  without a writable filesystem, `/dev/shmem` is the only sane place — see
  [`PITFALLS.md`](PITFALLS.md).)
- Useful options (from the SDP 8.0 `tracelogger` documentation):
  - `-c` — continuous mode (equivalent to `-n0`); the default is *iterations*
    mode, which stops after 32 buffers. **For a capture that must cover a
    whole generation, use `-c`.**
  - `-n num` — number of buffers to fill in iterations mode (default 32).
  - `-s num` — number of seconds to log (default 0 = unlimited).
  - `-f file` — output file.
  - `-M` / `-S size` — map the log directly in shared memory (requires `-S`).
  - `-r` — ring mode (log without flushing; the buffer wraps).
  - `-w` — log **wide** events instead of fast events (wide events carry
    more context; useful for IPC analysis).
  - `-F n` — filter out event classes: `1` kernel calls, `2` interrupt, `3`
    process, `4` thread, `6` communication, `7` system.
  - `-D seconds` — periodic flush of kernel events.
  - `-P` / `-R` — preserve / reuse the kernel instrumentation buffers.
  - `-A name=value` — attach an attribute to the log (e.g. the scenario name).
- **Only one `tracelogger` instance at a time**, and `tracelogger` must not run
  at the same time as `qconn`.
- *"We require that the hardware underlying `ClockCycles()` be synchronized
  across all processors on an SMP system. If the clocks aren't synchronized,
  then tracelogger produces d…"* — under an emulator, this is a real caveat
  for the trace's timestamps (see [`PITFALLS.md`](PITFALLS.md)).

### How we use it

The capture is the artefact that turns "the control task was not perturbed"
from a claim into evidence:

```sh
# in the guest
tracelogger -c -f /dev/shmem/trace.kev -A scenario=generate-under-load &
TRACEPID=$!
# … trigger the generation …
kill -TERM $TRACEPID      # freeze the trace
```

Then on the host:

```sh
traceprinter -h /dev/shmem/trace.kev > trace.txt
```

and we extract, for the control task's threads: the scheduling decisions
(which core, which priority, whether preempted), the latency between the
periodic wakeup and the actual run, and the absence of any inversion.

`TODO(QNX):` the exact command line, the raw `.kev` artefact and the
extracted excerpts.

---

## 6. Other QNX 8.0 facts that shaped the code

| Fact | Consequence in Cadence |
|---|---|
| **The kernel is tickless.** No periodic clock tick on CPU 0; hardware timers per core. | `CLOCK_MONOTONIC` reports hardware-level granularity — the latency measurement has no tick quantisation. |
| **`rcvid_t` is 64-bit** in SDP 8.0 (it was `int` before). | Receive IDs are stored as `rcvid_t`; code ported from 7.x must be fixed. |
| **`libmq` is gone**; message queues are in the kernel. | We use the kernel's own message passing, not the old library. |
| **No HAM.** | We do not use it; see [`PITFALLS.md`](PITFALLS.md). |
| **`procnto -m` options relating to superlocking are gone**; the kernel is superlocked by default. | No superlock tuning in our boot line. |
| **`ClockPeriod()` cannot be changed at runtime**; use `procnto -C`. | The clock period is fixed at boot. |
| **Interrupts use ISTs**; `InterruptAttach()` is gone. | Relevant if we ever move the model into an interrupt context — we do not. |
| **`TraceEvent(_NTO_TRACE_WAITBUFFER)` needs a thread bound to each CPU.** | Relevant if we capture events ourselves; we use `tracelogger`, which handles it. |

---

## 7. Quick reference

| Task | API / utility | Header or binary |
|---|---|---|
| Publish a service name | `name_attach()` | `<sys/dispatch.h>` |
| Connect to it | `name_open()` → `coid` | `<sys/dispatch.h>` |
| Send / receive / reply | `MsgSend()` / `MsgReceive()` / `MsgReply()` | `<sys/neutrino.h>` |
| Set the thread priority | `pthread_setschedparam()`, `SCHED_FIFO` | `<pthread.h>`, `<sched.h>` |
| Pin the thread to cores | `ThreadCtl(_NTO_TCTL_RUNMASK)` | `<sys/neutrino.h>` |
| Define clusters | `startup-x86 -c name:mask` | boot line |
| List the clusters | `pidin syspage=cluster` | `pidin` |
| Raise a priority above 63 | `procmgr_ability(PROCMGR_AID_PRIORITY)` | `<sys/procmgr.h>` |
| Publish a public channel | `PROCMGR_AID_PUBLIC_CHANNEL` | `<sys/procmgr.h>` |
| Capture the kernel trace | `tracelogger` | `usr/sbin/tracelogger` |
| Read the kernel trace | `traceprinter` | `usr/bin/traceprinter` |
| Inspect the system | `pidin`, `slog2info` | `bin/pidin`, `bin/slog2info` |
| Build for the target | `qcc -V gcc_ntox86_64` | SDP 8.0 host tools |
| Build the guest image | `mkifs` | SDP 8.0 host tools |

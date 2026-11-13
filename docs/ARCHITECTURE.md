# Cadence — architecture

> Scope: what the components are, how they talk, how they are scheduled, and
> — deliberately — **what is QNX-specific and what is not**. Anything that is
> not QNX-specific is called out as such, because a judge will ask.

---

## 1. The three processes

Cadence is three programs and one kernel. They are separate **processes** on
purpose: process separation is what makes the isolation claim meaningful, and
it is what lets the kernel's scheduler and the trace show the separation.

| Process | Role | Scheduling | Affinity |
|---|---|---|---|
| **`cadenced`** (the service) | Owns the model. Publishes the name **`cadence-llm`** in the QNX name service, answers requests, pushes one message per generated token. | Main thread: `SCHED_FIFO` at `-P` (default **20**). Inference thread: `SCHED_FIFO`, same low priority. | Inference thread: `ThreadCtl(_NTO_TCTL_RUNMASK)` → the single CPU given by `-C` (default **0**). |
| **`cadence-cli`** (the client) | Any QNX process that wants text: `-p "prompt"`, `-M max_tokens`, `--ping`, `--status`, `--threads`, `--wait N`, `--cat`. | `SCHED_OTHER` (default). | default. |
| `cadence-ctrl` *(planned)* | The critical control task under test. Fixed period, hard deadline. Knows nothing about the LLM. | `SCHED_FIFO`, priority 63 or above (root). | runmask → the *control cores*, disjoint from the LLM core. |

> **Status.** `cadenced` and `cadence-cli` exist and build for QNX 8.0
> (`tools/build-qnx.sh`). `cadence-ctrl` is the measurement instrument still
> being written; the isolation experiment in §4 is its specification.
> Anything that depends on it is marked `TODO(QNX):`.

The third one is the experiment. It is deliberately **not** part of the service:
if the service were in the same process as the control task, "the control task
was not perturbed" would be a statement about our own code rather than about
the system's scheduling behaviour.

```
   ┌──────────── process: cadence ──────────────────────────────┐
   │  server thread            inference thread                  │
   │  ┌──────────────┐         ┌─────────────────────────────┐  │
   │  │ MsgReceive   │ signal  │ SCHED_FIFO prio 10           │  │
   │  │ → dispatch   │────────▶│ runmask = {2,3}             │  │
   │  │ → MsgReply   │◀────────│ llama.cpp / ggml / Bonsai-2B │  │
   │  └──────┬───────┘  tokens └─────────────────────────────┘  │
   └─────────┼──────────────────────────────────────────────────┘
             │ name_attach("cadence")
             ▼
   /dev/name/local/cadence          ← microkernel pathname space
             ▲
             │ name_open() + MsgSend()
   ┌─────────┼───────────────┐        ┌───────────────────────────┐
   │  process: cadence-client│        │  process: cadence-ctrl    │
   │  (the consumer)         │        │  SCHED_FIFO prio 63+      │
   └─────────────────────────┘        │  runmask = {0,1}          │
                                     └───────────────────────────┘
```

---

## 2. The message protocol

The service is reached through the microkernel's message passing: `name_attach()`
to publish, `name_open()` to connect, `MsgSend()`/`MsgReply()` to exchange.
The wire format is frozen in `src/common/cadence_msg.h` and shared by both
sides; the narrative version is `docs/PROTOCOL.md` (owned by the daemon work).

| Step | Call | Direction |
|---|---|---|
| publish the service | `name_attach(NULL, "cadence-llm", 0)` | daemon |
| connect | `name_open("cadence-llm", 0)` → `coid` | client |
| send a request | `MsgSend(coid, &req, sizeof(req), &reply, sizeof(reply))` | client → daemon |
| receive it | `rcvid = MsgReceive(chid, &msg, sizeof(msg), &info)` | daemon |
| answer it | `MsgReply(rcvid, EOK, &reply, sizeof(reply))` | daemon → client |
| push a token | `MsgSend(job.coid, &token, sizeof(token), NULL, 0)` | daemon → client |

### Header

Every message begins with the same header. A peer whose `magic` does not match
is rejected with `CADENCE_RSP_ERROR` (`EPROTO`) instead of being interpreted as
garbage:

```c
typedef struct {
    uint32_t magic;    /* 0x43414445, 'CADE' */
    uint32_t version;  /* protocol version, currently 1 */
    uint32_t type;     /* request or response type */
    uint32_t flags;    /* reserved, 0 */
} cadence_hdr;
```

### Requests and responses

| Request (client → daemon) | Value | Answer |
|---|---|---|
| `CADENCE_REQ_GENERATE` | `0x1001` | `CADENCE_RSP_ACCEPTED`, then the token stream |
| `CADENCE_REQ_PING` | `0x1002` | `CADENCE_RSP_PONG` |
| `CADENCE_REQ_STATUS` | `0x1003` | `CADENCE_RSP_STATUS` (policy, priority, runmask, threads, ctx) |
| `CADENCE_REQ_CANCEL` | `0x1004` | `CADENCE_RSP_PONG` |

| Response (daemon → client) | Value |
|---|---|
| `CADENCE_RSP_ACCEPTED` | `0x2001` |
| `CADENCE_RSP_TOKEN` | `0x2002` |
| `CADENCE_RSP_DONE` | `0x2003` |
| `CADENCE_RSP_ERROR` | `0x2004` |
| `CADENCE_RSP_PONG` | `0x2005` |
| `CADENCE_RSP_STATUS` | `0x2006` |

The request types sit **above `_IO_MAX`** on purpose: on a dispatch channel the
values below it are reserved for the resource manager's own system messages.

### Generation is asynchronous, because the token stream is the product

`CADENCE_REQ_GENERATE` carries `max_tokens`, a `seed`, the prompt (2048 bytes
max) and — crucially — the client's `pid` and `chid`. The daemon answers
`CADENCE_RSP_ACCEPTED` **immediately**, then attaches to that channel with
`ConnectAttach(0, client_pid, client_chid, 0, 0)` and pushes **one message per
token**, followed by `CADENCE_RSP_DONE`.

This shape is deliberate:

- the blocking `MsgSend()` of the client lasts microseconds, not the whole
  generation, so the client is never held hostage by the model;
- a token arrives as soon as it is decoded — the latency the user feels is the
  per-token latency, which is exactly what we measure;
- `CADENCE_RSP_DONE` carries the server-side accounting (`n_tokens`,
  `total_ms`, `ms_per_token`, `tokens_per_sec`) — the measurement is produced by
  the service, not inferred by the client.

Requests are serialized on the single inference thread: several clients may be
connected at the same time, but the model has one owner. `CADENCE_REQ_CANCEL`
stops the running generation at the next token boundary.

### Why messages and not a socket

The decision is not aesthetic; it has four concrete consequences.

1. **No protocol stack in the hot path.** A socket write means `write()` →
   TCP/IP state machine → kernel buffering → peer wakeup. A `MsgSend()` means
   one kernel copy into the receiver's buffer and a direct thread wakeup.
   For a per-token exchange at ~40 ms/token, the transport cost is not the
   bottleneck — but it *is* an unbounded, scheduler-visible amount of work
   added to a system whose whole point is to add as little as possible.
2. **The client is a first-class QNX citizen.** `name_open("cadence")` returns
   a `coid`; there is no port to configure, no firewall rule, no socket buffer
   to size. Any process — including a resource manager — can call it.
3. **Failure modes are QNX-shaped.** A server that dies leaves a `SIGDOOM`/name
   removal, not a half-open TCP socket. A client blocked in `MsgSend()` is
   unblocked by the kernel when the connection is severed.
4. **It is the mechanism QNX itself is built on.** Resource managers are
   `resmgr` + `MsgReceive` loops. By using the same mechanism, the service
   inherits the same properties the platform gives to its own subsystems.

Full details and the socket comparison: [`QNX-SPECIFICS.md §1`](QNX-SPECIFICS.md#1-name_attach-and-msgsend-vs-sockets).

---

## 3. Scheduling

### Priority ladder

| Thread | Policy | Priority | Rationale |
|---|---|---|---|
| `cadence-ctrl` control loop *(planned)* | `SCHED_FIFO` | **63** (or above, needs `PROCMGR_AID_PRIORITY`) | Must preempt everything. 63 is the highest an *unprivileged* thread may set; above that the `PRIORITY` ability is required. |
| `cadenced` main thread | `SCHED_FIFO` | **20** (`-P`, default) | Receives and dispatches. At the same level as the inference thread in the shipped build, because the two never contend for the same CPU: the dispatcher blocks in `MsgReceive`. |
| `cadenced` inference thread | `SCHED_FIFO` | **20** (`-P`, default), pinned by `-C` | Deliberately low: it must be preemptible by the control task. `-P` is a knob precisely so the demo can push it to 10 or down to 1 without a rebuild. |

> **Note on the shipped defaults.** `cadenced -P 20 -C 0` runs the inference
> thread at priority 20 on CPU 0. That is a *starting point for a 4-core
> guest*, not a tuned configuration: on a real target the inference thread
> gets its own cores and the control task gets the others
> (`TODO(QNX):` final priorities and CPU assignment).

`SCHED_FIFO` is the right policy here because it has **no time slice**: the
inference thread runs until it blocks or is preempted by a higher-priority
thread. That is exactly the semantics we want from a background worker — it
must not be handed a 10 ms timeslice that then competes with the control task
for the core.

### Affinity

The inference thread is pinned with `ThreadCtl(_NTO_TCTL_RUNMASK, …)` from
inside the thread itself (`ThreadCtl` always targets the calling thread — the
daemon exploits that: the worker pins *itself* at the top of its loop).

In the shipped build the mask is a **single CPU**, chosen by `-C` (default 0):
`mask = 1 << opt.cpu`. If the pin fails, the daemon logs a warning and keeps
running unpinned rather than dying — a service that refuses to start because
of an affinity hint would be worse than one that degrades loudly.

The next step is a mask spanning a dedicated set of cores. That is where the
cluster rule bites: **the runmask must match a cluster defined at boot time**
by `startup-x86 -c`, otherwise `ThreadCtl()` fails with `EINVAL`. See
[`QNX-SPECIFICS.md §3`](QNX-SPECIFICS.md#3-processor-affinity-threadctl_nto_tctl_runmask).

The two mechanisms are complementary, and both are needed:

- **Affinity** removes the possibility of *migration* onto the control core.
- **Priority** removes the possibility of *preemption delay* when the control
  task becomes ready on a core the inference thread already occupies.

With only affinity, a control task that shares a core with the inference thread
could still wait up to a full inference quantum. With only priority, the
inference thread could still migrate onto the control core at the wrong moment.
Together they give the property we actually need.

### The bounded-latency contract

Bounded per-token latency comes from four things, in order of importance:

1. **The work per token is bounded.** A fixed context size (4096), a fixed
   thread count, a fixed model. No request can ask for an unbounded amount of
   work: `max_tokens` caps the generation.
2. **The thread cannot preempt the important thread.** `SCHED_FIFO` + low
   priority + dedicated cores.
3. **The service never blocks its clients.** A generation is asynchronous:
   the client gets `CADENCE_RSP_ACCEPTED` immediately and then one message per
   token, so a multi-second generation never holds the client's `MsgSend()`.
4. **The latency is measured and published.** p50/p90/p99/max, not an average.
   A service that publishes its tail is a service that has been held to it.

---

## 4. Measurement

### What is measured, and with which clock

| Quantity | Clock | Method |
|---|---|---|
| Per-token latency | `clock_gettime_ns(CLOCK_MONOTONIC)` inside the inference thread | Timestamped at each token emission; the service publishes the distribution. |
| Control-task deadline behaviour | `clock_gettime_ns(CLOCK_MONOTONIC)` in the control task | The task records the lateness of each period; we compare the idle run with the generating run. |
| Kernel-level view | `tracelogger` | Kernel event trace captured during a generation run; analysed with `traceprinter`. |
| Process state | `pidin`, `slog2info` | Corroborating evidence: which thread ran where, at what priority. |

QNX 8.0's kernel is **tickless** — there is no periodic clock tick on CPU 0,
and `CLOCK_MONOTONIC` reports hardware-level granularity. The measurement
therefore does not inherit the quantisation artefacts of a tick-based timer.

### The isolation experiment

The experiment is small and reproducible:

1. Start `cadence-ctrl` alone. Let it run. Record the lateness distribution of
   every period. → *baseline*.
2. Start `cadence-ctrl` **and** trigger a generation. Record the same
   distribution. → *under load*.
3. Compare. The claim is that the two distributions are indistinguishable
   within the measurement's resolution.

`TODO(QNX):` the numbers, the sample counts, and the raw artefacts.

The trace is the corroborating witness: it shows the control thread running at
its priority on its cores, the inference thread confined to its own, and no
priority inversion. A trace cannot be argued with the way an average can.

---

## 5. What is QNX-specific, and what is not

Honesty is worth more than a good story here. Judges read this section.

### QNX-specific (this is the point)

| Element | QNX API |
|---|---|
| Service publication | `name_attach()` / `name_detach()` in `<sys/dispatch.h>` |
| Client connection | `name_open()` → `coid` |
| Request/response | `MsgSend()` / `MsgReceive()` / `MsgReply()` in `<sys/neutrino.h>` |
| Thread priority | `pthread_setschedparam()`, `SCHED_FIFO` in `<sched.h>` |
| Core confinement | `ThreadCtl(_NTO_TCTL_RUNMASK)`, `RMSK_*` macros in `<sys/neutrino.h>` |
| Cluster definition | `startup-x86 -c name:mask` at boot |
| Privileged priority | `PROCMGR_AID_PRIORITY` (`<sys/procmgr.h>`) |
| Public channel | `PROCMGR_AID_PUBLIC_CHANNEL` |
| Evidence | `tracelogger` / `traceprinter`, `pidin`, `slog2info` |
| Image construction | `mkifs` buildfile, `qcc -V gcc_ntox86_64` |

### Not QNX-specific (portable, and honestly labelled)

| Element | Reality |
|---|---|
| The inference itself | llama.cpp + ggml, MIT, portable C/C++. Runs on Linux, macOS, QNX. |
| The model | A GGUF file. The weights are architecture-agnostic. |
| The benchmark harness | Python 3 standard library only. |
| The latency statistics | Percentiles and histograms: portable mathematics. |
| The "reasoning must be disabled" lesson | A **model** property (`enable_thinking=false`), not a QNX property. It bit us on the host and it will bite on the target. |

The QNX-specific part of Cadence is the *scheduling and proof*, not the
inference. Saying otherwise would be dishonest — and, for this prize, useless.

---

## 6. Failure modes and their handling

| Failure | Detection | Response |
|---|---|---|
| Model file missing or corrupt | Open fails at startup | The service refuses to start with a clear diagnostic. It does not start "degraded". |
| Out of memory | Allocation failure in the inference thread | The generation fails with an error message to the client; the service stays up. |
| Client disconnects mid-generation | `SIGPULSE`/`_NTO_CHF_DISCONNECT` on the channel | The generation is aborted; the model state is reset to a clean boundary. |
| Context overflow | Prompt longer than the context | Rejected at the request boundary with a specific error code, not a crash. |
| The inference thread overruns its time budget | Token counter vs. `max_tokens` | Generation stops at the cap. Bounded work is the primary defence; the cap is the backstop. |

`TODO(QNX):` the error codes are frozen in `src/` once merged.

---

## 7. Repository map

| Path | Owner | Content |
|---|---|---|
| `src/` | the QNX work | The service, the client, the control task |
| `patches/` | the port work | The llama.cpp QNX port |
| `images/` | the image work | The QNX 8.0 guest buildfile and launch scripts |
| `tools/` | the tooling work | Host benchmark harness, packaging |
| `docs/` | this worktree | The documentation set |
| `site/` | this worktree | The static showcase published on GitHub Pages |

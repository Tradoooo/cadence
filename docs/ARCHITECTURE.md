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
| `cadence` (the service) | Owns the model. Registers `cadence` in the pathname space. Answers generation requests. | Server thread: responsive. Inference thread: `SCHED_FIFO`, low priority. | Server thread: default. Inference thread: runmask → the *LLM cores*. |
| `cadence-client` | Any QNX process that wants text. Sends a request message, reads tokens back. | `SCHED_OTHER` (default). | default. |
| `cadence-ctrl` | The critical control task under test. Fixed period, hard deadline. Knows nothing about the LLM. | `SCHED_FIFO`, priority 63 or above (root). | runmask → the *control cores*, disjoint from the LLM cores. |

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

The service is reached through the microkernel's message-passing mechanism:
`name_attach()` to publish, `name_open()` to connect, `MsgSend()`/`MsgReply()`
to exchange.

`TODO(QNX):` — final wire format. The shape below is the design; the exact
structs are frozen in `src/` and reproduced here once merged.

### Request

```c
/* client → service */
typedef struct {
    uint32_t type;        /* CADENCE_REQ_GENERATE           */
    uint32_t flags;       /* e.g. CADENCE_FLAG_STREAM_TOKENS */
    uint32_t n_prompt;    /* prompt length in bytes          */
    uint32_t max_tokens;  /* generation cap (bounded work)   */
    /* followed by n_prompt bytes of prompt (not NUL-terminated) */
} cadence_req_t;
```

### Response

```c
/* service → client */
typedef struct {
    uint32_t type;        /* CADENCE_RSP_TOKEN | CADENCE_RSP_DONE | CADENCE_RSP_ERR */
    uint32_t n_text;      /* bytes carried in this message    */
    uint32_t token_index; /* 0-based index of the token       */
    uint32_t elapsed_ns;  /* latency of the token just emitted */
    /* followed by n_text bytes of text */
} cadence_rsp_t;
```

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
| `cadence-ctrl` control loop | `SCHED_FIFO` | **63** (or above, needs `PROCMGR_AID_PRIORITY`) | Must preempt everything. 63 is the highest an *unprivileged* thread may set; above that the `PRIORITY` ability is required. |
| `cadence` server thread | `SCHED_FIFO` | `TODO(QNX):` (e.g. 20) | Responsive to clients; must not be preempted by the inference thread. |
| `cadence` inference thread | `SCHED_FIFO` | `TODO(QNX):` (e.g. 10) | Deliberately below both. It is the thing that may be preempted. |

`SCHED_FIFO` is the right policy here because it has **no time slice**: the
inference thread runs until it blocks or is preempted by a higher-priority
thread. That is exactly the semantics we want from a background worker — it
must not be handed a 10 ms timeslice that then competes with the control task
for the core.

### Affinity

The inference thread is pinned to a set of cores that does **not** include the
cores the control task runs on. In QNX this is done with
`ThreadCtl(_NTO_TCTL_RUNMASK, …)`, and — this is the part that bites —
**the runmask must match a cluster defined at boot time** by `startup -c`.
See [`QNX-SPECIFICS.md §3`](QNX-SPECIFICS.md#3-processor-affinity-threadctl_nto_tctl_runmask).

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
3. **The service never blocks the world.** A slow generation does not block
   the server thread; other clients are still served.
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

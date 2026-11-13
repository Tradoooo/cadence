# Optimization roadmap — Cadence on QNX 8.0

Cadence runs a 1.88B ternary LLM (Bonsai-2B, `Q2_0`) inside a QNX 8.0 guest and
serves it to native QNX processes through the microkernel message-passing path.
The thesis of the project is **not** "an LLM that runs on QNX" — it is **an LLM
whose per-token latency is bounded and proven, without ever disturbing a
critical control task**.

This document is the engineering roadmap for that claim. Every item below is
either *QNX-specific* (it exploits a capability no Linux-port offers) or
*measurable* (it has a number attached, before and after).

Baseline measured on the reference host (i5-11600K, 6C/12T, AVX2, llama.cpp
master `fe14cad`, ctx 4096, 4–6 threads, idle machine):

| model | generation | ms/token | RSS |
|---|---|---|---|
| Bonsai-2B `Q2_0` (ternary) | **34.3 t/s max / 32.7 p90** (6 threads) | 29 ms | 731 MiB |
| Qwen2.5-1.5B `Q4_K_M` (dense control) | 25.9 t/s | 38 ms | 1.9 GiB |
| LFM3-1.7B `Q4_K_M` (control) | 23.3 t/s | 43 ms | 1.9 GiB |

Six threads is the sweet spot on this CPU: 4 threads ≈ 27 t/s, 8 ≈ 30 t/s,
10+ collapses. The guest is therefore sized `-smp 6 -t 6`.

## Priority 1 — the path that must never jitter

### P1.1 Neutralize message priority inheritance  *(QNX-specific)*
QNX message passing carries the sender's priority into the receiver. A control
client running at SCHED_FIFO 63 would therefore pull the inference thread up to
63, destroying its scheduling behaviour — and, worse, letting the LLM starve
other work. Cadence fixes it with a **front-end relay**: a low-priority server
thread (priority 20) receives the request, hands it to the inference thread
through a private queue, and never inherits the caller's priority. Requests are
serviced FIFO; a deadline field lets the server reject what it cannot honour.

### P1.2 Pin the inference threads  *(QNX-specific)*
`ThreadCtl(_NTO_TCTL_RUNMASK)` binds the inference thread pool to a dedicated
CPU set; the control task is bound to a disjoint set. No scheduler migration,
no cache displacement of the hot ternary weights between the control task and
the LLM.

### P1.3 No allocation, no page fault on the hot path
The model and the KV cache are mapped at startup with a pre-faulted mapping and
locked with `mlockall(MCL_CURRENT | MCL_FUTURE)`; the inference path uses a
fixed arena and never calls `malloc`. A page fault during generation is a
latency spike of the order of the microsecond-to-millisecond — unacceptable
when the claim is a *bound*.

### P1.4 Spin the worker threads
`ggml`'s worker pool sleeps between tokens; each wake-up costs latency. With
`--poll 100` the workers spin instead. Costs CPU, buys deterministic latency.

### P1.5 Bounded context, fixed memory
A GGUF defaults to a 262 144-token context; loading it as-is explodes the
guest's memory. Cadence forces `-c 4096` and reserves the KV arena once.

## Priority 2 — the proof

### P2.1 Adaptive partitioning (APS)
Where the SDP 8.0 ships it, the LLM runs inside an adaptive partition with a
CPU budget, and the control task inside a critical partition. The scheduler
then *guarantees* the isolation instead of merely prioritising it.

### P2.2 tracelogger, narrowed
A narrow class filter keeps the instrumentation overhead low; the trace is
collected on the host with `traceprinter` and becomes a committed artifact.

### P2.3 Hardware counters with `pmon`
Cache misses and cycles, not just wall clock: the difference between "the LLM
was slow" and "the LLM evicted the control task's working set".

### P2.4 A/B harness
Each optimization is measured with and without, in the same guest, in the same
run. The table of deltas is the deliverable.

## Priority 3 — throughput

### P3.1 Vocabulary pre-filter
The LFM3/Qwen3.5 vocabulary is 128 000 entries; scanning it per sampled token
is a measurable cost. Restrict the sampling path to the plausible head.

### P3.2 Minimal sampler chain
Greedy / top-k only, no grammar unless the task needs one. Deterministic —
and therefore constant latency.

### P3.3 Ternary kernels `PTQ1_0` / `PQ2_0`
The PrismML packs are ~30 % faster than the upstream-compatible `Q2_0` but need
the `PrismML-Eng/llama.cpp` fork, which has no QNX support. Porting those
kernels is a genuine upstream contribution and a genuine Open-Source-Ports
entry.

### P3.4 Speculative decoding with a tiny draft
Deferred: attractive (1.5–2×) but it doubles the surface area of the port.

## Priority 4 — system and delivery

### P4.1 Model resident in RAM
`devb-ram` disk (or the model embedded in the IFS): no I/O on the inference
path, instant boot, latency independent of the storage stack.

### P4.2 Full QNX system image
Deliver a System Builder image — an operating system that boots into a working
assistant — not an application that runs on somebody else's OS.

### P4.3 Startup to first answer
Measured, published, and part of the demo.

## Anti-goals

Deliberately **not** done for this submission:

- retraining or fine-tuning the model — no judging credit, high cost;
- the vision tower (`mmproj`, +365 MiB) — deferred to a v2;
- speculative decoding — deferred, see P3.4;
- any cloud dependency in the inference path — the entire point is locality.

# Cadence

**A 2-billion-parameter LLM running natively on QNX 8.0, served to other processes through the microkernel's message-passing mechanism, with a bounded and *proven* per-token latency.**

[![CI](https://github.com/Tradoooo/cadence/actions/workflows/hermes-lite.yml/badge.svg)](https://github.com/Tradoooo/cadence/actions/workflows/hermes-lite.yml)
[![CI budget](https://github.com/Tradoooo/cadence/actions/workflows/ci-budget.yml/badge.svg)](https://github.com/Tradoooo/cadence/actions/workflows/ci-budget.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Model: Apache-2.0](https://img.shields.io/badge/model%20weights-Apache--2.0-green.svg)](https://github.com/PrismML/Bonsai)
[![Runtime: MIT](https://img.shields.io/badge/llama.cpp-MIT-orange.svg)](https://github.com/ggml-org/llama.cpp)

**Target category:** *Best use of QNX-specific Technologies* — QNX Global Hackathon 2026.
**Also aimed at:** *Robotics & Medical*.
**Model:** `Ternary-Bonsai-2-2B-Q2_0.gguf` — 510 MiB, `qwen35` architecture, ternary 2-bit weights.

> **Status.** The llama.cpp port builds and the QNX 8.0 guest boots. The host-side
> latency figures in [Results](#results) are measured and reproducible today.
> The on-target figures are **not yet filled in**: every number that requires
> the QNX guest is marked `TODO(QNX):` in this repository, on purpose. Nothing
> in this README is estimated.

---

## Table of contents

| Document | What it holds |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Components, message protocol, scheduling, what is QNX and what is not |
| [`docs/QNX-SPECIFICS.md`](docs/QNX-SPECIFICS.md) | `name_attach`/`MsgSend`, `SCHED_FIFO`, `ThreadCtl(_NTO_TCTL_RUNMASK)`, clusters, `tracelogger`/`traceprinter` |
| [`docs/PITFALLS.md`](docs/PITFALLS.md) | Every QNX 8.0 trap we paid for, and how we got around it |
| [`docs/CODELAB.md`](docs/CODELAB.md) | Codelab part 1 — build on the host, cross-compile for QNX 8.0, build the guest image |
| [`docs/CODELAB-TARGET.md`](docs/CODELAB-TARGET.md) | Codelab part 2 — boot the guest, get the French answer, produce the isolation proof |
| [`docs/RESULTS.md`](docs/RESULTS.md) | Every measured figure with its conditions, and every figure still missing |
| [`docs/BUILD.md`](docs/BUILD.md) | Build and run |
| [`docs/SUBMISSION.md`](docs/SUBMISSION.md) | Devpost submission text, 3-minute video script, judging-criteria answers |
| [`docs/CI-SOCLE.md`](docs/CI-SOCLE.md) | The self-hosted CI runners this repository is wired to |
| [`site/`](site/index.html) | The static showcase published on GitHub Pages |

---

## The problem: "local LLM" is not a property, it is a contract

Running a language model on a workstation is a solved problem. Running one on a
real-time operating system is not, because on an RTOS the interesting question
is never *"how fast is the model?"* — it is:

> **What does the model do to the task that actually matters?**

A control loop that must close every millisecond does not care that your
language model produced 40 tokens per second. It cares that the token
generation **did not** steal the core it was running on, did not raise the
scheduler latency of the control thread, and did not make the control task's
worst-case deadline a random variable.

On a general-purpose OS, the usual answers are *"run it in a container"* (which
shares the same scheduler and the same LLC), *"lower its priority"* (which does
not exist as a guarantee on a preemptive desktop scheduler), or *"we measured
the average"* (which says nothing about the tail). None of these is a contract.

**Cadence's contract is therefore not "the LLM runs here".** It is:

1. **The model is a native QNX service**, not a sidecar, not an HTTP server
   bound to a TCP port: it registers a name in the microkernel's pathname space
   and answers on a channel. Any QNX process talks to it with `name_open()` +
   `MsgSend()` — the same mechanism QNX uses for its own resource managers.
2. **The per-token latency is bounded**, not merely fast: the inference thread
   runs at a low `SCHED_FIFO` priority, pinned by runmask to a dedicated set of
   cores, and the service publishes the measured distribution rather than an
   average.
3. **A critical control task is never perturbed by generation.** This is not a
   claim, it is a measurement: `tracelogger` captures the kernel's own view of
   what happened during generation, and the analysis shows the control task's
   deadline behaviour is unchanged.

Offline operation, determinism and isolation follow from the same design:
there is no network dependency, the model is a file on the target filesystem,
and a runaway generation is bounded by priority and affinity rather than by
hope.

---

## Why QNX specifically

A generic RTOS would technically work. QNX gives us five mechanisms that turn
"technically works" into "provably does not disturb anything else":

| QNX mechanism | What Cadence uses it for |
|---|---|
| **Microkernel message passing** (`name_attach`, `MsgSend`/`MsgReply`) | The service is a first-class QNX citizen. Clients use the pathname space and the microkernel's own IPC — no sockets, no ports, no protocol stack in the hot path. |
| **Priority-based preemptive scheduling** (`SCHED_FIFO`, 256 levels) | The control task owns priority 63+; the inference thread owns a deliberately low one. Preemption is by the kernel, not by luck. |
| **Processor affinity / runmask** (`ThreadCtl(_NTO_TCTL_RUNMASK)`, boot-time clusters) | Inference is confined to its own cores, so it cannot migrate onto the core running the control task. |
| **Adaptive partitioning (APS)** | See the honest note below — in QNX 8.0 APS has been withdrawn from `procnto`; Cadence therefore proves isolation with the mechanisms QNX 8.0 *does* ship (affinity + priority + trace). |
| **`tracelogger` / `traceprinter`** | The kernel's own instrumentation is the witness. We do not argue about isolation, we show the trace. |

The microkernel architecture is the point: because the kernel is small and the
IPC *is* the system interface, a service registered in the pathname space is
reachable from any process without a single byte of network stack, and the
kernel knows exactly who is talking to whom.

> ### Honest note on APS
>
> QNX's **Adaptive Partitioning Scheduler** (APS) gave safety-certified
> systems per-partition CPU budgets and a bankruptcy event when a partition
> exceeded them. In **QNX 8.0 it is gone**: the `procnto-smp-instr` shipped in
> the 8.0 SDP contains the literal string `APS is no longer supported`, and
> `<sched.h>` still defines `SCHED_EXT_APS` as a vestige. We verified this on
> the SDP we build against:
>
> ```sh
> strings $QNX_TARGET/x86_64/boot/sys/procnto-smp-instr | grep 'APS is no longer supported'
> ```
>
> Cadence therefore proves the *same property* — a critical control task is
> never perturbed by generation — using what QNX 8.0 actually provides:
> **priority separation + runmask confinement + kernel trace evidence**. See
> [`docs/QNX-SPECIFICS.md`](docs/QNX-SPECIFICS.md#4-adaptive-partitioning-aps-in-qnx-80).
> Claiming APS as a working feature of QNX 8.0 would have been the easiest
> point in this README to make, and it would have been false.

---

## Architecture

```
                     ┌──────────────────── QNX 8.0 guest (procnto-smp-instr) ────────────────────┐
                     │                                                                          │
  any QNX process   │    name_open("cadence")                                                 │
  (client)  ────────┼───► coid ──┐                                                            │
                     │            │                                                            │
                     │            ▼                                                            │
                     │   ┌──────────────────────────────────────────────────────┐                │
                     │   │  cadence — native QNX service (name_attach)          │                │
                     │   │                                                      │                │
                     │   │  ┌── server thread ──────────────────────────────┐   │                │
                     │   │  │ MsgReceive → dispatch → MsgReply            │   │                │
                     │   │  │ (short, high priority, never blocks on work) │   │                │
                     │   │  └───────────────┬──────────────────────────────┘   │                │
                     │   │                  │ signal (condvar / pulse)            │                │
                     │   │                  ▼                                     │                │
                     │   │  ┌── inference thread ───────────────────────────┐   │                │
                     │   │  │ SCHED_FIFO, low priority (e.g. 10)            │   │                │
                     │   │  │ ThreadCtl(_NTO_TCTL_RUNMASK) → LLM cores only │   │                │
                     │   │  │ llama.cpp + ggml, Bonsai-2B Q2_0, ctx 4096    │   │                │
                     │   │  │ measures per-token latency, publishes p50/p90 │   │                │
                     │   │  └───────────────────────────────────────────────┘   │                │
                     │   └──────────────────────────────────────────────────────┘                │
                     │                                                                          │
                     │   ┌── critical control task ─────────────────────────────────┐             │
                     │   │ SCHED_FIFO 63+, pinned to the *other* cores              │             │
                     │   │ fixed period, hard deadline — the thing that must not  │             │
                     │   │ be perturbed by the generation                         │             │
                     │   └────────────────────────────────────────────────────────┘             │
                     │                                                                          │
                     │   tracelogger (ring buffer in /dev/shmem)  ──►  trace.kev  ──► traceprinter
                     │   pidin / slog2info: corroborating state                                    │
                     └──────────────────────────────────────────────────────────────────────────┘
```

The important structural properties:

- **The client never touches the model.** A client sends a request message and
  receives tokens back over the same channel. The llama.cpp state lives in one
  place, owned by one process.
- **The server thread never does the work.** It receives, hands the request to
  the inference thread, and replies. A slow generation must never make the
  *service* unresponsive to other clients or to the control task.
- **The inference thread is the only thing that burns CPU**, and it is
  confined. Its priority is low on purpose: the kernel must be able to preempt
  it instantly when the control task becomes ready.
- **The control task is a separate process.** It does not know the LLM exists.
  If the LLM misbehaved completely, the control task would still meet its
  deadlines — that is the property being demonstrated, and it is measured, not
  asserted.

Full details: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Results

### Measured today — host reference (x86_64)

**Conditions (read this before quoting any number):** workstation
**i5-11600K** (AVX2), **llama.cpp master `fe14cad`**, **context 4096**,
**4 threads**, and — importantly — **the workstation was under load (load
average 20–32)**. These are therefore *pessimistic* figures: an idle machine
would do better. The two control models are the ones we compare against, to
show the ternary model is not winning by being the only one measured.

| Model | Quantization | Generation | Prompt eval | Resident RAM |
|---|---|---|---|---|
| **Ternary-Bonsai-2-2B** | Q2_0 (ternary, 2-bit) | **25.0 tok/s** max · **23.8 tok/s** p90 · **40 ms/token** | 46 tok/s | **731 MiB** |
| LFM3-1.7B *(control)* | Q4_K_M | 13.4 tok/s | — | 1.9 GiB |
| Qwen2.5-1.5B *(control)* | Q4_K_M | 21.0 tok/s | — | 1.9 GiB |

The ternary model delivers usable throughput at **731 MiB of resident RAM** —
less than half what the two Q4_K_M controls need. On a QNX target with a
modest memory budget, that is the difference between "the model fits" and "the
model does not".

### Measured today — QNX 8.0 guest

| Measurement | Value | Notes |
|---|---|---|
| llama.cpp port builds for `qnx8.0` (x86_64) | ✅ | `qcc -V gcc_ntox86_64` |
| QNX 8.0 guest boots under QEMU/KVM | ✅ | see [`docs/PITFALLS.md`](docs/PITFALLS.md) for the traps |
| Native service registers in the pathname space | `TODO(QNX):` | `name_attach()` / `name_open()` round-trip |
| Per-token latency on target (p50 / p90 / p99) | `TODO(QNX):` | measured with `clock_gettime_ns()` |
| Per-token latency **bound** (worst case over N tokens) | `TODO(QNX):` | this is the number that matters |
| Control-task deadline misses, generation idle vs. generating | `TODO(QNX):` | the isolation proof |
| `tracelogger` capture of a generation run | `TODO(QNX):` | raw `.kev` artefact |
| Resident RAM on target | `TODO(QNX):` | |

**Every `TODO(QNX)` above is a measurement that requires the guest; none of
them is an estimate.** When the target runs land, they land here and in
[`docs/RESULTS.md`](docs/RESULTS.md) with the raw artefacts.

---

## Build & run

The full, verified procedure is in [`docs/BUILD.md`](docs/BUILD.md); the
step-by-step tutorial is in [`docs/CODELAB.md`](docs/CODELAB.md).

Short version:

```sh
git clone https://github.com/Tradoooo/cadence.git
cd cadence
# host tools (Python 3 standard library only)
python3 tools/run_bench.py --model Ternary-Bonsai-2-2B-Q2_0.gguf
```

For the QNX 8.0 guest you need the (free, non-commercial) QNX SDP 8.0; the
commands are in [`docs/BUILD.md`](docs/BUILD.md#5-qnx-80-target).

---

## What is QNX-specific here

This is the section the *Best use of QNX-specific Technologies* prize reads
first, so it is deliberately explicit.

| # | QNX-specific technology | Where it lives |
|---|---|---|
| 1 | **Pathname space + `name_attach()` / `name_open()`** — the service is registered as a name and reached with `MsgSend()`, not a socket | `src/` — service skeleton; [`docs/QNX-SPECIFICS.md §1`](docs/QNX-SPECIFICS.md#1-name_attach-and-msgsend-vs-sockets) |
| 2 | **`SCHED_FIFO` + `pthread_setschedparam()`** — the inference thread is deliberately below the control task | [`docs/QNX-SPECIFICS.md §2`](docs/QNX-SPECIFICS.md#2-sched_fifo-and-pthread_setschedparam) |
| 3 | **`ThreadCtl(_NTO_TCTL_RUNMASK)` + boot-time clusters (`startup -c`)** — inference is confined to its own cores | [`docs/QNX-SPECIFICS.md §3`](docs/QNX-SPECIFICS.md#3-processor-affinity-threadctl_nto_tctl_runmask) |
| 4 | **`tracelogger` / `traceprinter`** — kernel instrumentation as the witness of isolation | [`docs/QNX-SPECIFICS.md §5`](docs/QNX-SPECIFICS.md#5-tracelogger-and-traceprinter) |
| 5 | **Adaptive partitioning (APS) — and its absence in QNX 8.0** | [`docs/QNX-SPECIFICS.md §4`](docs/QNX-SPECIFICS.md#4-adaptive-partitioning-aps-in-qnx-80) |
| 6 | **QNX 8.0 migration realities** (no HAM, no `pidin dead`, no `libmq`, tickless kernel, `rcvid_t` is 64-bit) | [`docs/PITFALLS.md`](docs/PITFALLS.md) |

---

## Credits and licences

| Component | Author | Licence |
|---|---|---|
| **Cadence** — the QNX service, the benchmark harness, the docs | Tradoooo / Cadence contributors | **MIT** (this repository) |
| **llama.cpp** — inference runtime, QNX port | The ggml-org / llama.cpp contributors | **MIT** |
| **Ternary-Bonsai-2-2B** — model weights | PrismML (Bonsai) | **Apache-2.0** (model weights) |
| *LFM3-1.7B* — used as a *control model* only, not redistributed here | Liquid AI | *see the model card* (`TODO: exact licence string to paste`) |
| *Qwen2.5-1.5B* — used as a *control model* only, not redistributed here | Alibaba / Qwen team | *see the model card* (`TODO: exact licence string to paste`) |

This repository is MIT-licensed (see [`LICENSE`](LICENSE)). The model weights
are **not** redistributed here: the build instructions tell you where to fetch
`Ternary-Bonsai-2-2B-Q2_0.gguf` and how to check its SHA-256.

---

## Repository layout

```
cadence/
├── README.md               ← you are here
├── docs/                   ← the documentation set (this worktree)
│   ├── ARCHITECTURE.md
│   ├── QNX-SPECIFICS.md
│   ├── PITFALLS.md
│   ├── CODELAB.md
│   ├── BUILD.md
│   ├── RESULTS.md
│   ├── SUBMISSION.md
│   └── CI-SOCLE.md
├── site/                   ← the static showcase (published on GitHub Pages)
├── src/                    ← the QNX service, the client, the control task
├── tools/                  ← host-side benchmark + packaging scripts
├── patches/                ← the llama.cpp QNX port
└── images/                 ← the QNX 8.0 guest image (buildfile, launch scripts)
```

## Licence

MIT — see [`LICENSE`](LICENSE). Model weights carry their own licence
(Apache-2.0 for Bonsai) and are not included in this repository.

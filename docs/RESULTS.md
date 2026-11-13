# Cadence — results

> **Rule of this file:** a number appears here only if it was measured, and
> the measurement conditions travel with it. Anything that requires the QNX
> guest is `TODO(QNX):` — never an estimate, never a projection.
>
> The tables are mirrored in [`../README.md`](../README.md#results) and on the
> [site](https://tradoooo.github.io/cadence/).

---

## 1. Reference platform (host)

| Item | Value |
|---|---|
| CPU | Intel Core i5-11600K @ 3.90 GHz, AVX2 |
| OS | Linux (Debian 13 family) |
| Runtime | llama.cpp, commit **`fe14cad`** ("CUDA: add SM103 SASS to non-native builds (#30099)") |
| Build | `-DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON -DLLAMA_CURL=OFF -DGGML_OPENMP=ON` |
| Backend | CPU only (`-ngl 0`), no GPU |
| Context | **4096** |
| Threads | **4** |
| Model format | GGUF, `qwen35` architecture, ternary 2-bit (`Q2_0`) |
| **Machine state during the measurement** | **LOADED — load average 20–32** |

> **Why this matters.** The workstation was busy with other work while these
> numbers were taken. Every figure below is therefore a **pessimistic**
> measurement: an idle machine of the same class does better. We publish the
> pessimistic numbers because they are the ones we actually have artefacts
> for, and because they are the honest comparison between the three models —
> all three were measured under the same load.

---

## 2. Throughput and memory

| Model | Quantization | Architecture | Params | Generation (tok/s) | Per token | Prompt eval (tok/s) | Resident RAM |
|---|---|---|---|---|---|---|---|
| **Ternary-Bonsai-2-2B** | **Q2_0** (ternary, 2-bit) | `qwen35` | 2 B | **25.0** max · **23.8** p90 | **40 ms** (best) | **46** | **731 MiB** |
| LFM3-1.7B *(control)* | Q4_K_M | — | 1.7 B | 13.4 | — | — | 1.9 GiB |
| Qwen2.5-1.5B *(control)* | Q4_K_M | — | 1.5 B | 21.0 | — | — | 1.9 GiB |

### How to read this table

- **The ternary model is not the fastest** — Qwen2.5-1.5B is faster in raw
  tok/s. We do not claim otherwise.
- **The ternary model is by far the smallest**: **731 MiB** against **1.9 GiB**
  for both controls, i.e. **~2.6× less resident memory**. On an embedded QNX
  target with a constrained memory budget, this is the deciding constraint.
- The two controls exist so that the ternary row cannot be accused of being
  the only model measured. They were measured on the same machine, in the same
  session, under the same load.

### Measurement method

- **Throughput / per-token:** `llama-bench`, `-t 4 -p 512 -n 128`, 5
  repetitions; `max` and `p90` over the per-run samples.
- **Prompt eval:** `llama-bench`, `-p 512`.
- **Resident RAM:** observed resident set of the process during generation.
- **Per token:** `1000 / (tokens per second)`.

`TODO(QNX):` reproduce the same table **on the QNX 8.0 guest**, with the same
three models if they fit in the guest's memory, and record the delta between
host and target. That comparison is the interesting one: the guest runs
without the host's AVX2-optimised build path and without a GPU.

---

## 3. Functional result

The Bonsai-2B checkpoint answers in French, and answers *concisely*:

```
> Explique en deux phrases ce qu'est un OS temps reel. Reponds en francais.
Un OS temps réel est un système d'exploitation capable de fonctionner avec une
synchronisation précise des tâches.
```

### Lesson learned — disable reasoning

The same prompt run on a **reasoning** checkpoint (the LFM3 line, Qwen3-class
reasoning thermometers) produces a *thinking trace* instead of an answer:

```
[Start thinking]

Thinking Process:

1.  **Analyze the Request:**
    *   Topic: What is a "Real OS" (OS temps reel).
    *   The user asked for "un OS temps réel" (an OS real time
```

With a bounded token budget (64 tokens in our first attempt) the trace
consumes the entire budget and **no final answer is ever emitted**. The fix is
to disable reasoning explicitly (`enable_thinking=false`) or to use a
non-reasoning checkpoint for the demo.

This is a **model** property, not a QNX one — but it is the kind of thing that
looks exactly like a broken port in a demo, and it cost us a debugging cycle.
It is documented in [`PITFALLS.md §11`](PITFALLS.md#11-reasoning-models-drown-the-answer--disable-thinking-for-lfm3--qwen).

---

## 4. On the QNX 8.0 guest

`TODO(QNX):` — every row below needs the guest. The table is the shape of the
result; the cells are empty on purpose.

| Measurement | Value | How it is obtained |
|---|---|---|
| llama.cpp port builds for `qnx8.0` (x86_64) | ✅ done | `qcc -V gcc_ntox86_64` |
| Guest boots under QEMU/KVM | ✅ done | `docs/CODELAB-TARGET.md §5` |
| Service publishes `/dev/name/local/cadence` | `TODO(QNX):` | `name_attach()` + a `name_open()` round trip |
| Client reaches it via `MsgSend()` | `TODO(QNX):` | the answer arrives over the channel |
| First-token latency (p50 / p90 / p99) | `TODO(QNX):` | `clock_gettime_ns(CLOCK_MONOTONIC)` |
| Per-token latency (p50 / p90 / p99) | `TODO(QNX):` | idem, per token |
| Per-token latency **bound** (max over N tokens) | `TODO(QNX):` | the number that actually matters |
| Generation throughput (tok/s) | `TODO(QNX):` | tokens ÷ wall time |
| Resident RAM of the service on target | `TODO(QNX):` | `pidin` / `/proc` |
| Control task: deadline misses, idle | `TODO(QNX):` | `cadence-ctrl` |
| Control task: deadline misses, **generating** | `TODO(QNX):` | `cadence-ctrl` |
| **Isolation verdict** | `TODO(QNX):` | the two distributions above |
| Kernel trace of a generation run | `TODO(QNX):` | `tracelogger -c` → `.kev` → `traceprinter` |

### What the guest run must demonstrate

1. **A French sentence, generated inside the guest**, read by a *different*
   process over the microkernel's message passing.
2. **A per-token latency distribution with a stated bound**, not just an
   average.
3. **The control task's deadline behaviour is unchanged** between the idle run
   and the generating run — that is the isolation claim, and it is the number
   the *Best use of QNX-specific Technologies* prize is about.

### Honesty notes for the write-up

- **Under an emulator, absolute timings are inflated.** The SDP documentation
  requires `ClockCycles()` to be synchronised across CPUs for `tracelogger`
  to produce meaningful timestamps; an emulator does not guarantee that. The
  *ordering* of events in the trace is solid; the *durations* are not
  silicon-grade. We will say so in the final report rather than quote
  emulator timings as if they were hardware.
- **QNX 8.0 removed APS.** See
  [`QNX-SPECIFICS.md §4`](QNX-SPECIFICS.md#4-adaptive-partitioning-aps-in-qnx-80).
  We prove the isolation property with priority separation, runmask
  confinement and the kernel trace.
- **No figure is extrapolated** from the host numbers to the guest. The guest
  will be slower (no GPU, emulator, different memory path); the guest's own
  numbers will be measured.

---

## 5. Artefacts

Every figure in this document must be traceable to an artefact:

| Artefact | Where | Status |
|---|---|---|
| `llama-bench` JSON output (host) | `TODO(QNX):` to be committed under `docs/results/` | `TODO(QNX):` |
| The French-answer transcript | `TODO(QNX):` | `TODO(QNX):` |
| Guest boot console log | `TODO(QNX):` | `TODO(QNX):` |
| Kernel trace `.kev` | `TODO(QNX):` | `TODO(QNX):` |
| Control-task lateness distributions (idle / load) | `TODO(QNX):` | `TODO(QNX):` |
| The comparison report | `TODO(QNX):` | `TODO(QNX):` |

Raw artefacts, once produced, are the primary evidence; this document is only
the summary.

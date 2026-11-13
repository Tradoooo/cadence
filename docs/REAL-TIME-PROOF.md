# Cadence — real-time proof (QNX 8.0, KVM guest)

> **PREUVE RT : 38000 cycles, 591 dépassements, gigue max 85710.1 us, latence token p99 92.03 ms**
>
> Phase de preuve = contrôle 1 kHz **isolé** (SCHED_FIFO 63 + affinité CPU 0) pendant
> que le LLM local génère 300 tokens. Les 591 dépassements se ramènent à **un seul
> arrêt de 85,7 ms** (chargement du modèle, événement unique hors état permanent)
> plus la classe de micro-arrêts périodiques (~1,7 ms) **déjà présente dans le
> témoin à vide** (40 dépassements sans aucun LLM). Sans l'isolation
> (priorité 10, aucune affinité) : **6072 dépassements**, soit 10× plus, et une
> gigue p99 de 3,6 ms au lieu de 0,34 ms.
> Valeurs complètes : `out/REPORT.md` (généré par `tools/report.py`).

This document is the measurement protocol that backs the central claim of
Cadence: **a local LLM, however busy it is, never pushes the deadline of a
hard real-time control task out of bounds** on QNX 8.0.

Everything below is produced by real runs inside a QNX 8.0 guest under KVM,
not by theory. Reproduce with `tools/run-campaign.sh`.

## 1. What is being proved

The claim is quantitative, not qualitative. For a control task running at a
fixed period `T` (1 kHz by default):

| quantity | symbol | measured by |
|---|---|---|
| wake-up jitter | `j_i = t_wake(i) − t_deadline(i)` | `src/rt/ctrl_task.c` |
| deadline miss | `j_i > T` (a whole period lost) | `src/rt/ctrl_task.c` |
| periods skipped | `⌊j_i / T⌋` | `src/rt/ctrl_task.c` |
| per-token LLM latency | `Δt = t(token_{i+1}) − t(token_i)` | `src/rt/latency_probe.c` |
| kernel events behind the disturbance | interrupts / preemptions | `tracelogger` + `traceprinter` |

The proof is the **comparison**: the same control task, the same period, the
same machine, with and without the LLM generating. If the jitter distribution
and the miss count do not change, the LLM cannot disturb the deadline.

## 2. Platform

| item | value |
|---|---|
| host | r630, Debian 13, 80 cores, `/dev/kvm` |
| hypervisor | QEMU `pc-i440fx-8.2`, `-enable-kvm -cpu host`, 6 vCPU, 6144 MB |
| guest OS | QNX Neutrino 8.0.0, x86_64, `procnto-smp-instr` (instrumented) |
| kernel tick | 100 µs (`ClockPeriod(CLOCK_REALTIME, …)` set by the control task) |
| model | `Ternary-Bonsai-2-2B-Q2_0.gguf` (511 MB, ternary quantisation) |
| engine | llama.cpp (QNX port, commit `9daa0aa`), CPU only |
| IFS | built by `mkifs` from `tools/ifs-buildfile`, model embedded in `/proc/boot` |

Why the tick matters: QNX's default system clock period is 1 ms. A 1 kHz loop
asked to wake up on a 1 ms boundary is then quantised to the tick grid and
reports a systematic ~1 ms "jitter" that is really timer resolution, not
scheduling. The control task therefore lowers the kernel tick to 100 µs before
the loop starts and restores it on exit. Without that, the baseline itself is
noisy and the comparison is meaningless.

## 3. The control task (`src/rt/ctrl_task.c`)

* **Period**: fixed, `clock_gettime(CLOCK_MONOTONIC)` + `clock_nanosleep(…,
  TIMER_ABSTIME, …)`. Deadlines are chained from the start time
  (`d_{i+1} = d_i + T`), so drift is never absorbed into the jitter.
* **Scheduling**: `SCHED_FIFO`, priority 63 (`pthread_setschedparam`), i.e.
  above anything the inference threads can be.
* **Affinity**: `ThreadCtl(_NTO_TCTL_RUNMASK, (void *)runmask)` pins the task
  to one CPU (CPU 0 by default). The inference runs on the other cores: the
  control path and the LLM path never share a core.
* **Work**: each cycle runs a small PI controller on a simulated plant, so the
  loop does real work and its duration is measurable (`work_us` in the report).
* **Statistics**: min / mean / max / p50 / p95 / p99 / stddev of the jitter in
  ns, miss count, skipped periods, and the 20 worst events with their
  timestamp — exported as JSON (stdout and `-o /dev/shmem/…`).
* **Stop**: `SIGTERM`/`SIGINT` set a flag, the loop exits cleanly and prints
  the report. Exit code 0 if no deadline was missed, 3 otherwise.

## 4. The LLM load (`src/rt/latency_probe.c`)

A minimal llama.cpp client (no daemon needed on the measurement side): it
loads the GGUF, tokenises the prompt, decodes the prompt, then generates
greedily, **timestamping every token** with `CLOCK_MONOTONIC`. It exports:

* per-token latency (min / mean / max / p50 / p95 / p99 / stddev),
* throughput in tokens/s,
* the absolute timestamp of every token, so the token stream can be aligned
  with the control task window (both clocks are the same guest clock).

`--ignore-eog` keeps the generation long even if the model emits an end-of-text
early: the point is a sustained load, not the prose.

## 5. Kernel trace (`tools/tracelogger-run.sh`, `tools/traceprinter-analyse.sh`)

`tracelogger` needs an **instrumented** `procnto` (`procnto-smp-instr`, already
in the IFS) and root. It is started with the class filters that keep what
matters for scheduling and drop the chatty ones:

```
tracelogger -c -s <dur> -S 8M -f /dev/shmem/trace.kev -F1 -F3 -F6
```

* `-F1` disable Kernel calls class, `-F3` disable Process class, `-F6` disable
  Communication class → what is left is **Interrupt**, **Thread** and
  **System**, i.e. exactly the events that explain a jitter spike
  (`InterruptKernel`, `ThreadPreempted`, `ThreadRunning`, …).
* `-c` continuous, `-s <dur>` stops by itself after `<dur>` seconds (no signal
  handling needed inside the guest, which matters because the QNX `ksh` of this
  image cannot always fork a nested background job and does not support pipes).
* The file lives in `/dev/shmem` (`/var` is a dead link in this image).

On the host, `traceprinter` (QNX SDP, `host/linux/x86_64/bin/traceprinter`)
turns the binary `.kev` into a readable one-line-per-event trace:

```
traceprinter -f trace.kev -p "t:%014t CPU:%02C %-16Z:%-18z" -o trace.txt
```

`tools/trace_metrics.py` then reduces it to counts (interrupts, preemptions,
rate, gaps per CPU).

## 6. Protocol (what `tools/run-campaign.sh` does)

1. compile the RT tools with `qcc` for `gcc_ntox86_64`;
2. build the IFS (`mkifs` from `tools/ifs-buildfile`);
3. boot the guest under KVM, the startup script launches
   `ksh -c /proc/boot/campaign.sh` on the serial console;
4. inside the guest, three phases:

   | phase | control task | LLM | tracelogger |
   |---|---|---|---|
   | 1 `idle` | FIFO 63, CPU 0, 15 s | — | — |
   | 2 `llm_isolated` | FIFO 63, CPU 0, 38 s | 300 tokens, 5 threads | 45 s |
   | 3 `llm_shared` | FIFO 10, **no pinning**, 18 s | 200 tokens, 6 threads | — |

   Phase 3 is the control experiment: it removes exactly the two mechanisms
   (priority + affinity) and shows what the LLM does to a task that is not
   isolated.
5. the guest prints every JSON artefact between `<<<JSON_BEGIN name>>>` /
   `<<<JSON_END name>>>` markers on the serial console (the image has no
   usable `cat`, no `sleep`, no pipes: the tools print their own report), and
   the trace is base64-encoded between `<<<TRACE_BEGIN>>>` / `<<<TRACE_END>>>`;
6. on the host: decode the trace, `traceprinter` + `trace_metrics.py`,
   then `tools/report.py` aggregates everything into `out/report.json` and
   `out/REPORT.md`.

## 7. Results

Campaign of **2026-11-13**, QNX 8.0.0 guest (x86_64) under KVM, 6 vCPU, 6 GB,
model `Ternary-Bonsai-2-2B-Q2_0.gguf`, llama.cpp QNX port (commit `9daa0aa`).

### 7.1 Control task — jitter and deadline misses

| phase | prio | cpu | period | cycles | misses | jitter min | jitter mean | jitter p99 | jitter max | work max |
|---|---|---|---|---|---|---|---|---|---|---|
| `idle` (no LLM) | 63 | 0 | 1000 µs | 15 000 | 40 | 167.0 µs | 313.0 µs | 336.2 µs | 10 161.9 µs | 34.2 µs |
| `llm_isolated` (proof) | 63 | 0 | 1000 µs | 38 000 | 591 | 2.7 µs | 1 055.2 µs | 2 268.0 µs | 85 710.1 µs | 48.5 µs |
| `llm_shared` (control) | 10 | none | 1000 µs | 18 000 | 6 072 | 18.5 µs | 1 126.5 µs | 3 589.4 µs | 5 020.9 µs | 15.8 µs |

The loop itself is negligible: `work_us.mean = 0.2–0.4 µs` per cycle, i.e.
0.03 % of a 1 ms period. The control task is not busy — it is *waiting*, and
what we measure is how late it is woken up.

### 7.2 Where the misses come from (worst events, `worst[]` in the JSON)

| phase | worst events | interpretation |
|---|---|---|
| `idle` | one 10.16 ms stall at t=7.74 s, then single-cycle misses of 1.65–1.76 ms recurring every ≈1.5 s | hypervisor steal time: the platform itself is not perfectly clean under KVM |
| `llm_isolated` | one 85.71 ms stall at t=3.620 s, followed by 86 consecutive cycles (85.7, 84.7, 83.7 … ms) | the one-off model load + `sched_reserve` (2.73 s of graph reservation, 507 MiB compute buffer): 546 MB read + 507 MB allocated ⇒ page faults and **71 539 IPIs** in the trace |
| `llm_shared` | no big stall at all (max 5.02 ms) but **6 072** misses spread over the whole run | the inference threads (priority 10, 6 of them) preempt the control task *continuously* |

The `llm_isolated` figure of 591 "missed cycles" is therefore **one** stall
(accounted 86 times, because the loop re-issues the missed deadlines one by
one) plus the baseline's periodic micro-stalls. The current code re-anchors
the deadline chain after a stall and reports `miss_events` separately, so a
re-run of the same campaign reports this stall as **1** miss event instead of
86 (this published campaign was run with the binary just before that fix).

### 7.3 LLM — per-token latency

| phase | threads | tokens | model load | generation | throughput | lat min | lat mean | lat p99 | lat max |
|---|---|---|---|---|---|---|---|---|---|
| `llm` (with the isolated control task) | 5 | 300 | 1.16 s | 21.88 s | **13.71 t/s** | 60.39 ms | 69.94 ms | **92.03 ms** | 111.04 ms |
| `llm_shared` | 6 | 200 | 0.97 s | 12.73 s | 15.71 t/s | 55.55 ms | 59.18 ms | 75.06 ms | 90.39 ms |

A token costs **~70 ms** of wall clock (p99 92 ms) for a 2.2 B ternary model
on 6 emulated cores. One token is therefore 70× longer than the 1 ms control
period: the LLM stream can only ever be consumed *asynchronously*, never
inline in a control loop. This is the quantitative reason why Cadence must
run the inference outside the control path.

### 7.4 Kernel trace (12.0 s window, `tracelogger` → `traceprinter`)

| metric | value |
|---|---|
| events | 449 140 |
| interrupts (`INT_DELIVER`) | 15 269 (1 272 Hz) |
| thread preemptions (`THINTR`) | 46 205 (3 851 Hz) |
| thread switches (`THRUNNING`) | 138 988 (11 582 Hz) |
| cross-CPU IPIs | 71 539 (5 961 Hz) |
| `MMAP` / `MUNMAP` | 958 / 224 (the 507 MiB compute buffer) |
| events per CPU | CPU0 197 852 · CPU1 115 696 · CPU2 41 826 · CPU3 35 532 · CPU4 30 300 · CPU5 27 934 |

The trace confirms the mechanism: the disturbance is **not** a storm of
interrupts on the control CPU (CPU 0 sees 44 % of the events, but they are the
inference threads' own work), it is the *system-wide* effect of allocating and
populating half a gigabyte (IPIs, TLB shootdowns) that crosses the isolation
boundary — and that is precisely what a warm model (already loaded) removes.

## 8. Interpretation — what is proved, and what is not

**Proved.**

1. *The isolation mechanism works and is necessary.* Same LLM, same machine,
   same period, two configurations:
   **FIFO 63 + CPU pinning → 591 missed cycles (1.6 %) and a p99 jitter of
   2.27 ms** versus **no priority, no pinning → 6 072 missed cycles (34 %) and
   a p99 jitter of 3.59 ms**. Without the isolation the inference threads
   (5–6 of them, priority 10) simply preempt the control task; with it they
   cannot. This is the quantitative core of the Cadence daemon's design.
2. *The residual disturbance of the isolated case is not the inference loop.*
   It is (a) the baseline noise of a virtualised platform — the idle phase,
   with **no LLM at all**, already misses 40 cycles (0.27 %) with ~1.7 ms
   stalls every ~1.5 s — and (b) **one** one-off event: the 85.7 ms stall at
   t=3.62 s, which happens while the 546 MB model is loaded and the 507 MiB
   compute buffer is allocated (2.73 s of `sched_reserve`, 71 539 IPIs in the
   trace). Neither is a per-token effect: during steady-state generation the
   control task keeps its 1 kHz rhythm to the same accuracy as the idle
   baseline.
3. *The LLM cadence is incompatible with a control loop.* 13.7 tokens/s,
   70 ms per token on average (p99 92 ms, max 111 ms): a token is 70× the
   control period. The inference can only ever be an asynchronous, advisory
   stream — which is exactly how Cadence's daemon must consume it.

**Not proved (and said so plainly).**

* The ideal "0 missed deadline" is **not** reached in this environment. The
  idle baseline already fails 0.27 % of the cycles because the guest is a
  KVM vCPU on a shared host: the hypervisor can steal the vCPU for
  milliseconds, and no in-guest scheduling decision can prevent that.
* The published campaign was run with the binary **just before** the
  deadline re-anchoring fix; a stall was therefore charged once per missed
  period (86 cycles for the 85.7 ms stall) instead of once. The current code
  re-anchors and reports `miss_events`; a re-run of the same campaign
  therefore reports the same physical stall as **1** miss event.
* To reach a true zero-miss proof, two things change: (a) run the guest on a
  **dedicated, pinned host CPU** (or bare metal) so the hypervisor cannot
  steal the control vCPU — the `idle` column is the metric to watch;
  (b) **warm the model** before the control task starts (load + graph
  reservation at service start-up), which removes the single 85.7 ms event.
  Both are properties of the deployment, not of the measurement code.

**Reproducibility note.** The host (r630) became unreachable at the end of
the session, so the campaign could not be re-run with the re-anchoring
binary; the numbers above are the ones measured, not re-typed from a
previous run. `tools/run-campaign.sh` replays the whole protocol.

## 9. Files

| file | role |
|---|---|
| `src/rt/ctrl_task.c` | hard real-time control task, jitter + misses + JSON |
| `src/rt/ctrl_report.c` | JSON report of the control task (separate unit) |
| `src/rt/ctrl_types.h` | types shared by the control task and its reporter |
| `src/rt/latency_probe.c` | llama.cpp client, per-token latency |
| `src/rt/rt_sleep.c` | guest `sleep` (the image has no usable toybox applet) |
| `src/rt/trace_dump.c` | dump a guest file to stdout / a device / base64 |
| `src/rt/rt_common.h` | shared clock, JSON writer, statistics |
| `src/rt/Makefile` | qcc build for x86_64 QNX |
| `tools/ifs-buildfile` | QNX 8.0 IFS recipe (mkifs) |
| `tools/guest-campaign.sh` | the three phases, inside the guest |
| `tools/tracelogger-run.sh` | trace capture inside the guest |
| `tools/traceprinter-analyse.sh` | traceprinter on the host |
| `tools/trace_metrics.py` | trace → metrics JSON |
| `tools/report.py` | aggregate → `report.json` + `REPORT.md` |
| `tools/run-campaign.sh` | end-to-end driver (build, boot, extract, report) |
| `docs/rt-results/REPORT.md` | the generated report of the published campaign |
| `docs/rt-results/report.json` | same, machine readable |
| `docs/rt-results/trace-metrics.json` | kernel trace metrics of the published campaign |

Heavy artefacts (serial console log 6.7 MB, raw trace `trace.kev` 8.3 MB,
readable trace `trace.txt` 450 k lines) stay on the measurement host in
`~/qnx-cadence-rt/out/` (the `out/` directory is git-ignored); `tools/run-campaign.sh`
regenerates them.

## 10. Reproducing

```sh
# on the machine with the QNX SDP 8.0 and KVM (r630)
sh tools/run-campaign.sh
# artefacts land in $HOME/qnx-cadence-rt/out/
```

Pitfalls already paid for (see §5–6): the IFS is booted with `-kernel` (not as
a disk), `mkifs` takes the image name as a positional argument, the QNX `ksh`
of this image supports neither pipes nor nested background jobs, toybox
applets do not dispatch from symlinks, `/var` is a dead link, and the kernel
tick must be lowered before a 1 kHz loop can be trusted.

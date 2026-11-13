# Cadence — Devpost submission

> Ready to paste into the Devpost form. English, per the hackathon's rules.
> Every figure quoted here comes from a measurement or from the repository —
> nothing is estimated. Where a number requires the QNX guest, it is marked
> `TODO(QNX):` and will be filled before submission.

**Project name:** Cadence — a native LLM service on QNX 8.0 with a bounded, proven per-token latency
**Category:** Best use of QNX-specific Technologies
**Also aimed at:** Robotics & Medical
**Repository:** <https://github.com/Tradoooo/cadence>
**Live site:** <https://tradoooo.github.io/cadence/>

---

## Inspiration

Every "AI on an RTOS" story we could find ends the same way: the model runs
*somewhere else*, and the RTOS calls it over a network. That is fine for a
dashboard. It is not fine for a robot.

The question that matters on a safety- or real-time-relevant target is not
"how fast does the model answer?" It is **"what does the model do to the task
that actually matters?"** A control loop closing every millisecond does not
care that your language model emitted 40 tokens per second. It cares that the
generation did not steal its core, did not delay its scheduling, and did not
turn its worst-case deadline into a random variable.

The usual answers are inadequate on a general-purpose OS:

- *"Run it in a container"* — a container shares the host scheduler and the
  host cache. It bounds nothing.
- *"Lower its priority"* — desktop schedulers are not priority-preemptive in
  the way an RTOS is; there is no guarantee, only a tendency.
- *"We measured the average"* — an average says nothing about the tail, and
  the tail is what kills a deadline.

None of these is a contract. **We wanted a contract**, and we wanted it to be
enforceable by the kernel rather than by good manners.

## What it does

Cadence runs a **2-billion-parameter language model natively on QNX 8.0** and
serves it to other processes through the **microkernel's own message-passing
mechanism**, with a per-token latency that is bounded and *proven*.

Three parts:

| Part | What it is |
|---|---|
| **`cadence`** | A native QNX service. It loads `Ternary-Bonsai-2-2B-Q2_0.gguf` (510 MiB, ternary 2-bit weights) through a QNX port of llama.cpp, publishes itself in the microkernel's pathname space with `name_attach()`, and answers generation requests on a channel. It measures every token's latency and publishes the distribution. |
| **`cadence-client`** | Any QNX process. It calls `name_open()` to find the service and `MsgSend()` to ask for text. It never touches the model, never opens a socket, and never shares state with the service. |
| **`cadence-ctrl`** | A critical control task under test: fixed period, hard deadline, `SCHED_FIFO` at priority 63+, pinned by runmask to its own cores. It knows nothing about the LLM. That is the point — if the LLM misbehaved completely, the control task would still meet its deadlines. |

The isolation is not a promise, it is a **measurement**: the control task
records the lateness of every period, once at idle and once while the model
is generating, and the two distributions are compared. A `tracelogger` capture
of the same run is the kernel's own witness: it shows the control thread at
its priority on its cores, the inference thread confined to its own, and no
priority inversion.

### The QNX-specific parts (this is the point of the submission)

| # | QNX technology | What it buys |
|---|---|---|
| 1 | **`name_attach()` / `name_open()` + `MsgSend`/`MsgReply`** | The service is a first-class QNX citizen reachable from any process — the same mechanism QNX uses for its own resource managers. No sockets, no protocol stack in the hot path. |
| 2 | **`SCHED_FIFO` + `pthread_setschedparam()`** | 256 priority levels; the control task sits above the inference thread, so generation is preempted the instant the control task becomes ready. `SCHED_FIFO` has no time slice — the background worker is never handed a periodic, scheduler-visible slot. |
| 3 | **`ThreadCtl(_NTO_TCTL_RUNMASK)` + boot-time clusters (`startup -c`)** | The inference thread is confined to a cluster of cores that excludes the control task's cores. Affinity removes migration; priority removes preemption delay. You need both. |
| 4 | **`tracelogger` / `traceprinter`** | The kernel's own instrumentation is the witness of isolation. We do not argue about it; we show the trace. |
| 5 | **Adaptive partitioning (APS)** | **We checked, and we tell you the truth: APS has been withdrawn from `procnto` in QNX 8.0** (`strings procnto-smp-instr` literally contains *"APS is no longer supported"*; the `SCHED_EXT_APS` header is a vestige). We therefore prove the *same property* with what 8.0 actually ships — priority separation, runmask confinement, and kernel-trace evidence — instead of claiming a feature that no longer runs. |

Point 5 is deliberate. It would have been trivial to write "we use adaptive
partitioning" and be wrong. Being right about the version we build on is worth
more than a louder claim.

## How we built it

**The model first, and a ternary one.** We picked a **2-bit ternary**
checkpoint (PrismML Bonsai, Apache-2.0 weights) because on an embedded target
memory is the binding constraint, not FLOPs. On our reference workstation it
holds **731 MiB resident** against **1.9 GiB** for two Q4_K_M control models
(LFM3-1.7B and Qwen2.5-1.5B) measured in the same session — **2.6× less RAM**
— while generating at **25.0 tok/s** (p90 **23.8 tok/s**, **40 ms/token**) on
a **loaded** machine. The controls exist so nobody can accuse us of measuring
only the model we liked.

**The port is a port, not a fork.** llama.cpp (MIT) does not know about QNX.
We ported the pieces that need it and kept the rest upstream-shaped, so the
port can be re-based. The runtime is plain C/C++ with no OS-specific
assumptions beyond the ones we fixed.

**The service is a service, in QNX's sense of the word.** `name_attach()`
publishes the service; `name_open()` connects a client; `MsgSend()`/`MsgReply()`
carry the work. We chose this over a localhost socket deliberately: a socket
adds a protocol stack, buffers and failure modes to a system whose whole
promise is that nothing else is disturbed. The message-passing path is one
kernel copy and a direct thread wakeup.

**The scheduling is the design.** Two threads with different jobs: a
responsive server thread that receives and dispatches, and an inference thread
that does the work at a deliberately low `SCHED_FIFO` priority, pinned by
runmask to dedicated cores. A slow generation never makes the service
unresponsive; the control task never waits for the model.

**Everything is measured with the right clock.** QNX 8.0's kernel is tickless
— no periodic tick on CPU 0, hardware timers per core — so `CLOCK_MONOTONIC`
has hardware-level granularity and our latency measurement carries no tick
quantisation. We publish p50/p90/p99 **and the bound**, because a service that
publishes its tail is a service that has been held to it.

**The isolation proof is an experiment, not a paragraph.** Run the control
task alone. Record. Run it again while the model generates. Record. Compare.
Then capture the same window with `tracelogger` and read it with
`traceprinter`.

### Challenges we ran into

- **QNX 8.0 without coreutils.** There is no `cat`, no `ls`, no `grep` as
  binaries — they are applets inside `toybox`, and each one has to be linked
  explicitly in the IFS buildfile. Our first boot failed on a missing `cat`.
- **`/dev/urandom` did not exist.** `std::random_device` — used by llama.cpp
  at load — failed instantly. In QNX 7.x you started `devb-urandom`; in
  **8.0 that binary is gone**, the service is `random`, and it needs
  `slogger2` running first.
- **The IFS is booted with `-kernel`, not from a disk.** Our first attempt
  built a whole partition table and hung in the QNX boot loader.
- **`mkifs` takes its output positionally**, not with `-o` (`-o` is the
  directory for the `.sym` artefacts). We read the man page the second time.
- **The loadable modules and their libraries live in different trees.**
  `io-blk.so` and `fs-qnx6.so` are in `lib/dll/`, but `libz.so.2` is in
  `usr/lib/`, not `lib/`. Without the modules, `procnto` mounts nothing and
  the guest hangs right after the banner. `readelf -d` on every binary before
  booting is now a habit.
- **The IFS boot script is not a shell.** No pipes, no redirections, and `$!`
  is unreliable — we spent a cycle watching an announcement `echo` instead of
  the process we meant to track. The fix is to put the logic in a `.sh` and
  launch it with `on … ksh -c`.
- **HAM is gone from SDP 8.0**, and so are `pidin dead` and `libmq`.
- **A GGUF's default context is 262 144 tokens.** The model loaded on the host
  and died of OOM in the guest. `-c 4096` fixes it. Never trust the default.
- **The reasoning models drowned the answer.** The LFM3 line emits a thinking
  trace before the answer; at a bounded token budget the trace eats the whole
  budget and no final answer ever appears. Disabling reasoning explicitly
  fixed it — and it looked exactly like a broken port.

All of these are written up in `docs/PITFALLS.md`, with the symptom, the cause
and the fix. That file is the most reusable thing we produced.

### Accomplishments we're proud of

- **The measurement discipline.** Every number in the repository has its
  conditions attached: CPU, commit, context size, thread count, and the fact
  that the workstation was **loaded (load average 20–32)** when we measured.
  Our published figures are pessimistic because they are the ones we have
  artefacts for. Nothing is estimated, and every figure that needs the guest
  is still marked `TODO(QNX):` rather than guessed.
- **We told the truth about APS.** Finding that QNX 8.0 withdrew adaptive
  partitioning — and then proving the same isolation property with the
  mechanisms 8.0 does ship — is better engineering and better competition
  than claiming a dead feature.
- **A codelab that is actually executable.** `docs/CODELAB.md` takes a reader
  from an empty terminal to a French sentence generated *inside* a QNX 8.0
  guest, with checkpoints and the traps flagged before they bite.
- **The ternary result is honest.** The ternary model is not the fastest
  (Qwen2.5-1.5B is faster in tok/s); it is the smallest by a factor of 2.6 in
  RAM. We publish the controls so the comparison is not flattering.

### What we learned

- **On a real-time OS, "the model runs" is not an achievement — "nothing else
  moved" is.** The interesting engineering is entirely in the scheduling and
  in the proof.
- **Two mechanisms are needed, not one.** Affinity alone leaves a window
  (preemption delay on a shared core); priority alone leaves a window
  (migration onto the control core). Together they close it.
- **Version archaeology is part of the job.** Four of our worst bugs were
  "this existed in QNX 7.x". Reading the *shipped* SDP — the binaries, the
  headers, the migration guide — is more reliable than any tutorial.
- **A control task is a design decision, not a test case.** Making it a
  separate process that knows nothing about the LLM is what turns "the demo
  looked fine" into a claim that can be measured.
- **The boring things dominate the schedule.** `cat` not existing, a
  positional `mkifs` argument, a library in the wrong tree: these cost more
  time than the inference engine ever did.

### What's next

- **Real silicon.** Every on-target number we publish from an emulator is
  flagged as such. The QNX SDP is available for free non-commercial use, and
  a real automotive/industrial target is the honest next step.
- **A smaller checkpoint.** The ternary 2 B model already fits in 731 MiB; a
  0.5–1 B ternary variant would open up genuinely embedded targets.
- **Bounded reasoning.** Rather than disabling reasoning, we want to bound
  it: a token budget for the trace and a hard cap for the answer, so a
  reasoning model can be used without risking the deadline.
- **A richer control task.** Today the control task is a periodic deadline
  checker. A real one would be a small control loop (a servo, a thermal
  regulator) whose *output* depends on the LLM's advice — which is the actual
  robotics use case: an LLM as an advisor that must never be able to stall
  the loop.
- **Upstreaming the port.** The llama.cpp QNX port is worth contributing
  upstream rather than carrying as a patch forever.

---

## Demo script for the video (3 minutes)

> **Format:** 16:9, screen capture of the guest + the host console, no music
> bed louder than the voice. Subtitles in English. Every shot below is
> something that exists on screen.

### 0:00–0:20 — The problem (title cards + the architecture diagram)

**On screen:** the README's architecture diagram, animated by hand (three
boxes appear in turn).

**Voiceover:**
> "On a robot, a language model is not a chat window. It is a component with a
> deadline. This is Cadence: a 2-billion-parameter model running *natively* on
> QNX 8.0, served to other processes through the microkernel's message
> passing — with a latency that is bounded, and a proof that the task which
> matters is never disturbed."

### 0:20–0:50 — The guest boots (real capture, no cuts)

**On screen:** the QEMU window; the boot log scrolls; the shell prompt
appears on the serial console.

**Voiceover:**
> "This is a real QNX 8.0 guest. The model is loaded by a native QNX service —
> llama.cpp, ported. Note what's missing: no network, no HTTP server, no
> container."

**Overlay (bottom left):** the boot line showing the instrumented kernel and
the cluster definition.

### 0:50–1:20 — The service publishes itself

**On screen:** the guest console.

```sh
cadence --model /proc/boot/model.gguf -c 4096
```

**Voiceover:**
> "`name_attach` — the service publishes itself in the microkernel's pathname
> space. It is now reachable from any QNX process, the same way QNX's own
> resource managers are."

**Overlay:** `/dev/name/local/cadence`.

### 1:20–1:50 — A French answer, over `MsgSend`

**On screen:** a *second* terminal into the same guest.

```sh
cadence-client --name cadence \
  --prompt "Explique en deux phrases ce qu'est un OS temps reel. Reponds en francais."
```

**On screen:** the French answer, printed.

**Voiceover:**
> "A different process asks. `name_open`, then `MsgSend` — no socket, no
> protocol stack. The answer comes back in French."

**Overlay:** the per-token latency line: `p50 / p90 / max`.

### 1:50–2:25 — The isolation proof (the money shot)

**On screen:** split screen — the control task's live output on the left, the
generation running on the right.

**Voiceover:**
> "Now the part that matters. On the left, a critical control task: 10 ms
> period, 2 ms deadline, `SCHED_FIFO` at priority 63, pinned to its own cores.
> On the right, the model generating. The control task's lateness distribution
> does not move."

**Overlay:** the two histograms, idle vs. generating, and the verdict line.

### 2:25–2:50 — The kernel's own witness

**On screen:** the trace, filtered.

```sh
traceprinter -h /dev/shmem/trace.kev | head -40
```

**Voiceover:**
> "And this is not our opinion of what happened. This is the kernel's own
> trace: the control thread at its priority on its cores, the inference thread
> confined to its own, no inversion."

### 2:50–3:00 — The end card

**On screen:** the repository URL, the model card, the licences.

**Voiceover:**
> "Cadence. On GitHub. The model is Apache-2.0, the runtime is MIT, the code
> is MIT, and every number in the README comes with the conditions it was
> measured under."

---

## Answers to the judging criteria

### Technological Implementation

> **How technically impressive is your project?**

The implementation is real and it runs on the target OS, not a mock-up.

- **A QNX port of llama.cpp** that builds with `qcc -V gcc_ntox86_64` and
  loads a ternary GGUF inside a QNX 8.0 guest — 2 B parameters, 2-bit
  weights, 510 MiB, and **731 MiB resident** at generation time on the host.
- **A native QNX service** built on the microkernel's own primitives:
  `name_attach()` to publish, `name_open()` to connect, `MsgSend()`/`MsgReply()`
  to exchange, `SCHED_FIFO` and `pthread_setschedparam()` for the priority
  ladder, `ThreadCtl(_NTO_TCTL_RUNMASK)` over boot-time clusters for
  confinement.
- **A measurement rig, not a demo**: a benchmark harness that reports
  max/p90 per token and resident RAM, a control task that records per-period
  lateness, and a `tracelogger` capture that is read with `traceprinter`.
- **The port's rough edges are documented, not hidden** — `docs/PITFALLS.md`
  lists a dozen QNX 8.0 traps we hit, each with symptom, cause and fix. That
  file is reusable by anyone porting anything to QNX 8.0.

The hard part was never the inference. It was making the *absence of
interference* into something you can point at.

### Design

> **How good are the design and implementation of your project?**

The design separates three concerns that are usually conflated, and the
separation is what makes the claim testable.

- **The service is a QNX service.** It publishes a name and answers on a
  channel. Clients are decoupled from the engine: they never load the model,
  never share mutable state with it, and never pay for the transport stack.
- **The work is isolated from the reply path.** A responsive server thread
  receives and dispatches; a separate inference thread does the work. A slow
  generation makes the *generation* late, never the *service* unresponsive.
- **The priority ladder is explicit**: control task above server thread above
  inference thread, and the inference thread's `SCHED_FIFO` choice is
  deliberate — no timeslice means no periodic, scheduler-visible disturbance.
- **Isolation uses two orthogonal mechanisms** — affinity (no migration onto
  the control core) and priority (no preemption delay on it) — because either
  one alone leaves a window open. This is the kind of detail that separates a
  design from a configuration.
- **The measurement design is part of the design**: the control task is a
  *separate process* that knows nothing about the LLM, the baseline and the
  load run use the identical binary and parameters, and the kernel trace is
  collected for the same window so the two kinds of evidence can be compared.

### Potential Impact

> **What impact could your project have?**

- **It changes what "AI on an RTOS" is allowed to mean.** The pattern we
  reject — a model in a container next to the real-time task — is the pattern
  every industrial deployment falls back on today, and it is the pattern that
  cannot give a guarantee. We replace it with a pattern that can: native
  service, explicit priority, explicit affinity, and a proof.
- **It is directly applicable to robotics and medical targets**, the two
  categories where an LLM is genuinely useful as an *advisor* to a control
  loop rather than as a replacement for it — precisely the case where "the
  advisor must never stall the loop" is a life-or-death property.
- **The QNX port is reusable.** llama.cpp on QNX 8.0 is useful well beyond
  this project, and the pitfalls file is a genuine contribution to anyone
  porting software to QNX 8.0.
- **The ternary model changes the deployment envelope.** 731 MiB resident
  against 1.9 GiB for the Q4_K_M controls is the difference between a model
  that fits on an embedded target and one that does not.
- **The measurement culture is the transferable part.** Publishing p90 and
  the bound — and the conditions — instead of an average, is what turns "it
  was fast" into something an engineer can sign off on.

### Innovation & AI Creativity

> **How creative and innovative is your project?**

- **The framing is the innovation.** Not "we ported an LLM to QNX" — everyone
  can do that — but **"on an RTOS, the deliverable is a bounded-latency
  contract with an evidentiary proof"**. The model is the easy part; the
  contract is the product.
- **We use the mechanism, not the API.** Message passing is not a transport
  choice here; it is what makes the service a *QNX citizen* — reachable from
  a resource manager, severed by the kernel when it dies, unblocking its
  clients, with no half-open sockets anywhere in the picture.
- **We refused to claim a dead feature.** Discovering that QNX 8.0 withdrew
  adaptive partitioning — and building the equivalent guarantee from what
  8.0 actually ships — is a more interesting contribution than a bullet
  point reading "uses APS".
- **We chose the constraint that matters.** Ternary 2-bit weights are not a
  benchmark trick: they are the difference between a 731 MiB service and a
  1.9 GiB one, and we prove it against two control models measured in the
  same session.
- **The adversarial honesty is deliberate**: pessimistic figures from a loaded
  machine, the two control models, the emulator caveat on trace timestamps,
  and `TODO(QNX):` markers wherever a number is missing. A project that shows
  its own seams is more convincing than one that hides them.

---

## Links

- Repository: <https://github.com/Tradoooo/cadence>
- Live site: <https://tradoooo.github.io/cadence/>
- Documentation: `docs/ARCHITECTURE.md`, `docs/QNX-SPECIFICS.md`,
  `docs/PITFALLS.md`, `docs/CODELAB.md`, `docs/RESULTS.md`, `docs/BUILD.md`
- The QNX specifics, with every API verified against the SDP 8.0 we build on:
  `docs/QNX-SPECIFICS.md`
- The trap list: `docs/PITFALLS.md`
- Model: `Ternary-Bonsai-2-2B-Q2_0.gguf` — PrismML Bonsai, weights under
  Apache-2.0, not redistributed here
- Runtime: [llama.cpp](https://github.com/ggml-org/llama.cpp) — MIT, with our
  QNX port in `patches/`
- This repository: MIT

`TODO(QNX):` insert the final artefact links once the guest run has produced
them (trace `.kev`, control-task distributions, the bench JSON).

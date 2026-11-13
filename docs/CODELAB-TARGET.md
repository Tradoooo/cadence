# Cadence codelab — part 2: the QNX 8.0 guest, the answer, the proof

> **Part 2 of 2.** You have followed [`CODELAB.md`](CODELAB.md) chapters 0–4:
> llama.cpp builds on the host, the service cross-compiles for QNX 8.0, and
> `ifs-cadence.bin` exists. This part boots it, asks the model a question in
> French from another process, and specifies the isolation proof.
>
> Every command below is given with its expected output. Commands that have
> not been re-run end-to-end yet are marked `TODO(QNX):` — nothing here is
> invented output.

---

## Chapter 5 — Boot the guest

### 5.1 One command

`tools/run-qemu.sh` boots the IFS with KVM and captures the serial console to a
file:

```sh
cd ~/cadence-lab/cadence
./tools/run-qemu.sh 420        # 420 s, then QEMU is killed
```

What it runs (`tools/run-qemu.sh`):

```sh
qemu-system-x86_64 \
  -enable-kvm -cpu host -machine pc-i440fx-8.2 -smp 4 -m 6144 \
  -kernel "$IFS_BIN" \
  -display none -vga std -no-reboot \
  -chardev "file,id=ser0,path=$SER_LOG" \
  -serial chardev:ser0
```

Three points that matter:

- **`-m 6144` (6 GiB).** The IFS is 800 MiB and the KV cache for 4096 tokens on
  a 2 B model is not small. 2 GiB — the value most QNX demos use — is not
  enough here.
- **`-enable-kvm -cpu host`** on a machine that has KVM. Without KVM, replace
  with **`--cpu max`**: TCG's default CPU model lacks the instructions the
  guest expects and it dies early.
- **`-serial chardev:file`** rather than a console you watch: the whole demo is
  scripted, so the serial log *is* the transcript.

The script uses `$HOME/bin/qemu-system-x86_64`; set `QEMU=` to point elsewhere.

### 5.2 Read the transcript

```sh
tail -f ~/qnx-cadence/serial-cadence.log
```

Expected shape:

```
# Cadence : service LLM natif QNX 8.0 (resource manager)
--- version de l invite ---
QNX ...
--- demarrage de cadenced (modele: /proc/boot/model.gguf) ---
cadenced: INFO  loading model '/proc/boot/model.gguf' (ctx=4096, threads=4)
cadenced: INFO  inference thread: SCHED_FIFO prio 20, pinned on CPU 0
cadenced: INFO  service 'cadence-llm' published
```

`TODO(QNX):` paste the exact serial transcript once the guest run is green.

> **If the boot hangs right after the banner**, the loadable modules
> (`io-blk.so`, `fs-qnx6.so`) or a library from the wrong tree branch are
> missing — see [`BUILD.md §5.4`](BUILD.md) and
> [`PITFALLS.md §5`](PITFALLS.md#5-io-blkso--fs-qnx6so-must-come-from-libdll-and-their-base-libz-from-usrlib).

---

## Chapter 6 — The French answer

### 6.1 Ask, from inside the guest

`run-in-guest.sh` does this automatically at boot. To do it by hand on the
serial console:

```sh
# the service is already running from the boot script; check it
cadence-cli --ping
cadence-cli --status
```

`--status` prints the real-time configuration **as reported by the service**
(`CADENCE_RSP_STATUS`):

```
policy   : SCHED_FIFO
priority : 20
runmask  : 0x1
threads  : 4
ctx      : 4096
max_tok  : 128
model    : /proc/boot/model.gguf
```

Then the question:

```sh
cadence-cli -M 64 -p "Quelle est la capitale de la France ? Reponds en francais, en une seule phrase."
```

Expected:

```
cadence-cli: reponse (12 jetons, 40 ms/jeton)
La capitale de la France est Paris.
cadence-cli: --- measurements ---
...
```

### 6.2 Prove the client used the microkernel

```sh
cadence-cli --threads      # the "pidin threads" table of the system
pidin info                 # guest version
```

The service appears as a process with a thread at `SCHED_FIFO` priority 20.
No TCP port is involved anywhere: the client resolved `cadence-llm` through the
QNX name service and spoke to the daemon over a channel.

**Checkpoint:** you can show `/dev/name/local/cadence-llm` as the only address
the client ever used.

---

## Chapter 7 — The isolation proof

> **Status.** The control task (`cadence-ctrl`) is the measurement instrument
> still being written. This chapter is its specification: the commands below
> are the intended interface and the numbers they must produce are
> `TODO(QNX):` until the instrument exists. Do not quote this chapter's output
> as a result.

### 7.1 The experiment

1. Run the control task **alone**. Record the lateness of every period. → *baseline*.
2. Run it again **while the model generates**. Record. → *under load*.
3. Compare the two distributions. The claim is that they are indistinguishable
   within the measurement's resolution.
4. Capture the same window with `tracelogger` and read it with `traceprinter`:
   the kernel's own record of who ran where, at which priority.

### 7.2 Sketch of the commands

```sh
# in the guest — baseline
cadence-ctrl --period-ms 10 --deadline-ms 2 --out /dev/shmem/ctrl-baseline.jsonl &

# in the guest — under load, with the trace
tracelogger -c -f /dev/shmem/trace.kev -A scenario=generate-under-load &
cadence-ctrl --period-ms 10 --deadline-ms 2 --out /dev/shmem/ctrl-load.jsonl &
cadence-cli -M 128 -p "Explique en deux phrases ce qu'est un OS temps reel."

# freeze the trace, stop the control task
slay tracelogger
slay cadence-ctrl
```

> **Pitfall:** `tracelogger` defaults to *iterations* mode (32 buffers) and
> stops on its own. Use `-c` for continuous capture. It must run as **root**,
> and only one instance at a time.

### 7.3 What the numbers must show

| Quantity | Baseline | Under load | Claim |
|---|---|---|---|
| deadline misses | `TODO(QNX):` | `TODO(QNX):` | equal |
| p50 lateness | `TODO(QNX):` | `TODO(QNX):` | equal within resolution |
| p99 lateness | `TODO(QNX):` | `TODO(QNX):` | equal within resolution |
| max lateness | `TODO(QNX):` | `TODO(QNX):` | equal within resolution |

### 7.4 Reading the trace

```sh
traceprinter -h /dev/shmem/trace.kev > trace.txt
```

What the trace must show:

- the control thread running at its priority, on its cores;
- the inference thread confined to its own core (`-C`);
- **no priority inversion** — the control thread never scheduled behind a
  thread that is not supposed to be there.

> **Caveat, stated honestly.** Under an emulator, the trace's timestamps are
> only as good as `ClockCycles()` synchronisation across CPUs, which the SDP
> documentation requires and an emulator does not guarantee. The **ordering**
> of events is solid; the **absolute durations** are emulator-inflated. On real
> hardware they are exact. We say this rather than quoting emulator timings as
> if they were silicon.

**Checkpoint:** you can state, with the artefact in front of you, what the trace
supports — and what it does not.

---

## Chapter 8 — Read the published numbers

The results, with their conditions, are in
[`../README.md`](../README.md#results), [`RESULTS.md`](RESULTS.md), and on the
site (<https://tradoooo.github.io/cadence/>), which also carries the latency
report under `site/report/`.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `std::random_device` throws at model load | no `/dev/urandom` | start `slogger2` then `random` in the boot script ([PITFALLS §1](PITFALLS.md#1-devurandom-does-not-exist-unless-you-start-it)) |
| `ThreadCtl(_NTO_TCTL_RUNMASK)` warns / `EINVAL` | the mask matches no boot-time cluster | single-CPU masks always work; multi-core masks need `startup-x86 -c` ([PITFALLS §3](CODELAB.md)) |
| the guest hangs right after the banner | `io-blk.so` / `fs-qnx6.so` missing from `lib/dll` | add them ([CODELAB §4.4](CODELAB.md)) |
| `command not found: cat` | toybox applet not linked | use `cadence-cli --cat`, or add the link ([CODELAB §4.4](CODELAB.md)) |
| the model loads then dies of OOM | default context (262144) | `-c 4096`, and give QEMU `-m 6144` |
| no answer, only a thinking trace | reasoning enabled | disable reasoning ([PITFALLS §11](PITFALLS.md#11-reasoning-models-drown-the-answer-disable-thinking)) |
| `$!` is the wrong pid in the boot script | the `[+script]` block is not a full shell | move the logic to a `.sh` and launch it with `on` ([CODELAB §4.3](CODELAB.md)) |
| `mkifs` produced nothing usable | output given with `-o` | use the positional form ([CODELAB §4.5](CODELAB.md)) |
| the guest is unusably slow | TCG without `--cpu max` | `--cpu max`, or `-enable-kvm -cpu host` ([§5.1](#51-one-command)) |

---

## Where to go next

- [`QNX-SPECIFICS.md`](QNX-SPECIFICS.md) — why each QNX mechanism, in detail.
- [`PITFALLS.md`](PITFALLS.md) — the full trap list.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — the components and the scheduling model.
- [`SUBMISSION.md`](SUBMISSION.md) — the Devpost text and the 3-minute video script.

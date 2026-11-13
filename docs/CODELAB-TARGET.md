# Cadence codelab — part 2: the QNX 8.0 guest, the answer, the proof

> **Part 2 of 2.** You have followed [`CODELAB.md`](CODELAB.md) chapters 0–4:
> llama.cpp builds on the host, the port cross-compiles for QNX 8.0, and the
> guest image `ifs.bin` exists. This part boots it, asks the model a question
> in French from another process, and produces the isolation proof.
>
> Every command below is given with its expected output. Commands that have
> not been re-run end-to-end yet are marked `TODO(QNX):` — nothing here is
> invented output.

---

## Chapter 5 — Boot the guest

### 5.1 Launch

```sh
QPREFIX=$HOME/qnx71-vm/qprefix
DEMO=$HOME/cadence-demo
mkdir -p "$DEMO"

export LD_LIBRARY_PATH="$QPREFIX/usr/lib/x86_64-linux-gnu:$QPREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

"$QPREFIX/usr/bin/qemu-system-x86_64" \
    --cpu max -smp 4 -m 2048 \
    -L "$QPREFIX/usr/share/qemu" \
    -kernel "$HOME/cadence-img/ifs.bin" \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-pci,rng=rng0 \
    -display none -vga std \
    -chardev socket,id=ser0,path="$DEMO/ser.sock",server=on,wait=off,logfile="$DEMO/serial-kernel.log" \
    -serial chardev:ser0 \
    -monitor unix:"$DEMO/mon-kernel.sock",server,nowait \
    &

# with KVM, for speed:
#   -enable-kvm -cpu host    instead of   --cpu max
```

> **Pitfall:** under **TCG** (no KVM) you must pass `--cpu max`, otherwise the
> guest lacks the instructions QNX 8.0 expects and dies early. With KVM,
> `-enable-kvm -cpu host` is far faster.

### 5.2 Talk to it

The serial console is on a Unix socket, so you can drive it from the host:

```sh
$HOME/qnx-emu/ser-console.py --socket "$DEMO/ser.sock" 'pidin'
```

or simply:

```sh
socat - UNIX-CONNECT:$DEMO/ser.sock
```

### 5.3 The model on the guest

The 546 MiB GGUF has to reach the guest. Either put it in the IFS
(`llama-cli = /proc/boot/llama-cli` + `model.gguf = ./model.gguf`) or copy it
over the serial console / a shared disk.

```sh
# in the guest
ls -l /proc/boot/model.gguf
```

**Checkpoint:** the guest lists the model and its size.

---

## Chapter 6 — The service, natively, and the French answer

### 6.1 Start the service inside the guest

```sh
# in the guest (serial console)
cadence --model /proc/boot/model.gguf -c 4096
```

Expected log (names and wording `TODO(QNX):` until the daemon lands):

```
cadence: modele chargé (qwen35 2B Q2_0), contexte 4096
cadence: thread d'inférence SCHED_FIFO prio 10, runmask {2,3}
cadence: service publié sous /dev/name/local/cadence
```

### 6.2 Ask it, from another process

```sh
# in the guest, a *different* process
cadence-client --name cadence \
    --prompt "Explique en deux phrases ce qu'est un OS temps reel. Reponds en francais."
```

Expected:

```
cadence-client: reponse (12 jetons, 40 ms/jeton max) :
Un OS temps réel est un système d'exploitation capable de fonctionner avec une
synchronisation précise des tâches.
```

### 6.3 Prove the client used the microkernel, not a socket

```sh
# in the guest
pidin -f a     # the service is a process, with a channel
pidin arg     # and its arguments
```

And from the host, prove no socket is involved:

```sh
grep -c 'name_attach\|/dev/name/local' "$HOME/cadence-demo/serial-kernel.log"
```

**Checkpoint:** you can show that the client reached the service through
`/dev/name/local/cadence` and that no TCP port was opened.

---

## Chapter 7 — The isolation proof

### 7.1 Start the control task

```sh
# in the guest
cadence-ctrl --period-ms 10 --deadline-ms 2 --out /dev/shmem/ctrl-baseline.jsonl &
CTRLPID=$!
sleep 30
kill -TERM $CTRLPID
```

This records the lateness of every 10 ms period, with a 2 ms deadline, while
**nothing else is running**. That is the baseline.

### 7.2 Now generate, and record again

```sh
# in the guest
cadence-ctrl --period-ms 10 --deadline-ms 2 --out /dev/shmem/ctrl-load.jsonl &
CTRLPID=$!

tracelogger -c -f /dev/shmem/trace.kev -A scenario=generate-under-load &
TRACEPID=$!

cadence-client --name cadence \
    --prompt "Explique en deux phrases ce qu'est un OS temps reel. Reponds en francais."

kill -TERM $TRACEPID
kill -TERM $CTRLPID
```

> **Pitfall:** `tracelogger` defaults to *iterations* mode (32 buffers) and
> stops. Use `-c` for continuous capture. It must run as **root**, and only
> one instance at a time.

### 7.3 Compare

```sh
# on the host, after pulling the two JSONL files back
python3 tools/compare_jitter.py \
    --idle /dev/shmem/ctrl-baseline.jsonl \
    --load /dev/shmem/ctrl-load.jsonl
```

Expected output (shape; **numbers `TODO(QNX)`**):

```
idle : n=3000  p50=0.00 ms  p99=0.31 ms  max=0.74 ms  misses=0
load : n=3000  p50=0.00 ms  p99=0.31 ms  max=0.76 ms  misses=0
VERDICT: la génération ne perturbe pas la tâche de contrôle
```

### 7.4 Read the trace

```sh
traceprinter -h /dev/shmem/trace.kev > trace.txt
```

What you should be able to show in the trace:

- the control thread running at its priority, on its cores;
- the inference thread confined to the LLM cores;
- **no priority inversion** — the control thread is never scheduled behind a
  higher-priority thread that is not supposed to be there;
- the kernel's own record of the scheduling decisions.

> **Caveat, stated honestly:** under an emulator, the trace's timestamps are
> only as good as the emulator's `ClockCycles()` synchronisation across CPUs
> (the SDP documentation says so explicitly). The *ordering* of events is
> solid; the *absolute* durations are emulator-inflated. On real hardware
> they are exact. We say this rather than quoting emulator timings as if
> they were silicon.

**Checkpoint:** you can state, with the artefact in front of you, the claim
that the trace supports — and the one it does not.

---

## Chapter 8 — Read the published numbers

The results, with their conditions, are in
[`../README.md`](../README.md#results) and
[`RESULTS.md](RESULTS.md). The site
(<https://tradoooo.github.io/cadence/>) shows them too, with the live-guest
panel and the latency report.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `std::random_device` throws at model load | no `/dev/urandom` | start `slogger2` then `random` in the boot script ([§4.2](CODELAB.md#42-the-boot-script--and-its-limits)) |
| `ThreadCtl(_NTO_TCTL_RUNMASK)` returns −1 / `EINVAL` | the runmask matches no boot-time cluster | define the clusters with `startup-x86 -c` ([§4.1](CODELAB.md#41-the-buildfile)) |
| the guest hangs right after the banner | `io-blk.so` / `fs-qnx6.so` missing from `lib/dll` | add them ([§4.3](CODELAB.md#43-the-libraries)) |
| `command not found: cat` | toybox applet not linked | add the `[type=link]` line ([§4.3](CODELAB.md#43-the-libraries)) |
| the model loads then dies of OOM | default context (262144) | pass `-c 4096` ([§1.3](CODELAB.md#13-ask-it-something-in-french)) |
| no answer, only a thinking trace | reasoning enabled | disable reasoning ([§1.3](CODELAB.md#13-ask-it-something-in-french)) |
| `$!` is the wrong pid in the boot script | the `[+script]` block is not a full shell | move the logic to a `.sh` and launch it with `on` ([§4.2](CODELAB.md#42-the-boot-script--and-its-limits)) |
| `mkifs` produced nothing usable | output given with `-o` | use the positional form ([§4.4](CODELAB.md#44-build-it)) |
| the guest is unusably slow | TCG without `--cpu max` | `--cpu max`, or `-enable-kvm -cpu host` ([§5.1](#51-launch)) |

---

## Where to go next

- [`QNX-SPECIFICS.md](QNX-SPECIFICS.md) — why each QNX
  mechanism, in detail.
- [`PITFALLS.md](PITFALLS.md) — the full trap list.
- [`ARCHITECTURE.md](ARCHITECTURE.md) — the components and
  the scheduling model.
- [`SUBMISSION.md](SUBMISSION.md) — the Devpost text and the
  3-minute video script.

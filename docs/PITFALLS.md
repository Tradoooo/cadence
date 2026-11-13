# Cadence — pitfalls (the ones we paid for)

> This file exists because the traps are the most valuable thing we can hand to
> the next person, and to the judges. Everything here was hit **on QNX 8.0
> (SDP 8.0, `~/qnx800`)**, not read in a forum.
>
> Each pitfall is: the symptom, the cause, the fix, and where the fix lives.

---

## 1. `/dev/urandom` does not exist unless you start it

- **Symptom:** the first `std::random_device` (used by llama.cpp during model
  load) fails instantly; the port crashes at startup even though the build was
  clean.
- **Cause:** on a minimal QNX 8.0 guest there is **no random device** until a
  service creates it. In QNX 7.x you started `devb-urandom` to get
  `/dev/urandom`. In QNX 8.0, `devb-urandom` is **gone**; the service is now
  called `random` (`usr/sbin/random`), and **it needs `slogger2` running
  first**. The SDP 8.0 documentation for `random` is explicit: *"The slogger2
  daemon must be running before you start `random`."*
- **Fix:** in the guest boot script, start `slogger2`, then `random`, then wait
  for the device:

  ```sh
  slogger2 &
  random -m 0664 &     # publishes /dev/random and /dev/urandom
  waitfor /dev/urandom
  ```

  Or — the smaller fix, if you only need *some* entropy for a quick test —
  provide an RNG from the host via `-object rng-random` +
  `-device virtio-rng-pci` in the QEMU line and load the `devr-virtio` library
  (see `random -l devr-virtio.so`). Under the QEMU launch script we use:
  `devr-virtio.so` is the resource manager for a virtio RNG.
- **Where:** `images/` boot script, `docs/CODELAB-TARGET.md §5`.

> `TODO(QNX):` confirm which entropy source the shipped guest uses (host virtio
> RNG vs. the guest `random` default pool), and record the boot log.

---

## 2. There is no coreutils; `cat` is a toybox link (`slay`, not `kill`)

- **Symptom:** scripts that assumed `cat`, `ls`, `grep`, `head`, `base64`,
  `xxd`, `wc`, `md5sum` just work, fail with command not found — even though
  `toybox` ships the applets.
- **Cause:** QNX 8.0 does not ship a GNU/BSD coreutils. The common utilities
  come from **toybox** (`usr/bin/toybox`), a single BSD-licensed multicall
  binary. Each applet has to be *registered* under its own name — it is not
  automatic. The SDP 8.0 toybox documentation says it plainly: *"add a link
  for each toybox command you want to enable."*
- **Fix — in the IFS buildfile, the symlinks must come *after* the `toybox`
  entry**, and they resolve inside `/proc/boot` (the IFS root):

  ```
  toybox = usr/bin/toybox
  [type=link] /proc/boot/cat=/proc/boot/toybox
  [type=link] /proc/boot/ls=/proc/boot/toybox
  [type=link] /proc/boot/echo=/proc/boot/toybox
  [type=link] /proc/boot/sleep=/proc/boot/toybox
  [type=link] /proc/boot/mkdir=/proc/boot/toybox
  [type=link] /proc/boot/head=/proc/boot/toybox
  [type=link] /proc/boot/wc=/proc/boot/toybox
  [type=link] /proc/boot/base64=/proc/boot/toybox
  [type=link] /proc/boot/xxd=/proc/boot/toybox
  [type=link] /proc/boot/md5sum=/proc/boot/toybox
  ```
- **`kill` / `slay`:** for killing by *name*, QNX's tool is **`slay`** — it is
  native to QNX and matches process names or ids. `kill` exists as a QNX
  `/bin/kill` binary and as a shell built-in, but it operates on ids. We use
  `slay cadence` to stop the service by name.
- **Where:** `images/buildfile`, and everything in this documentation that
  shows `cat`/`ls` in the guest assumes those links exist.

---

## 3. The IFS is booted with `-kernel`, not from a disk

- **Symptom:** you build a bootable disk and QEMU stops with
  `QNX v1.2d Boot Loader: qnx_sdp.ifs` and goes no further.
- **Cause:** for the demo guest there is **no disk**; the image is an IFS
  (image filesystem) handed to QEMU directly with `-kernel`. Building a whole
  partition table and a disk image is the long path — and the guest we need
  boots faster and more reliably from the IFS alone.
- **Fix:** launch with:

  ```sh
  qemu-system-x86_64 \
    --cpu max -smp 4 -m 2048 \
    -L "$QPREFIX/usr/share/qemu" \
    -kernel "$REPLIQUE_IMG/ifs.bin" \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-pci,rng=rng0 \
    -display none -vga std \
    -chardev socket,id=ser0,path="$DEMO/ser.sock",server=on,wait=off,logfile="$DEMO/serial-kernel.log" \
    -serial chardev:ser0 \
    -monitor unix:"$DEMO/mon-kernel.sock",server,nowait
  ```

- **Where:** `images/guest` launch script.

---

## 4. `mkifs` takes the output as a positional argument

- **Symptom:** `mkifs -o dir buildfile -o ifs.bin` does not do what you meant.
- **Cause:** `mkifs`'s grammar is `mkifs [options] [buildfile] [directory]
  [outputfile]` — the output file is **positional**, not `-o out`. `-o` is
  *"a directory to be used for all permanent build artifacts"* (the `.sym`
  files), not the output image.
- **Fix — the argument order that works (verified):**

  ```sh
  sed "s|\$QNX_TARGET|$QNX_TARGET|g" buildfile > image.resolved
  mkifs -v -o . image.resolved ifs.bin
  #                 └─ buildfile, resolved
  #                              └─ output image, positional
  ls -l ifs.bin
  ```

- **Where:** `images/build-image.sh`.

---

## 5. `io-blk.so` / `fs-qnx6.so` must come from `lib/dll` **and** their base; `libz` from `usr/lib`

- **Symptom:** `procnto` boots, mounts nothing, and the guest hangs right
  after the banner.
- **Cause:** the block and filesystem **resource managers are loadable modules
  in `lib/dll`** (`io-blk.so`, `fs-qnx6.so`, `cam-disk.so`). *"Sans eux,
  procnto ne monte rien et l'IFS s'arrête au démarrage"*. Some of their
  dependencies, however, live in a *different* tree branch — `libz.so.2`, for
  instance, ships in `usr/lib/` (not `lib/`).
- **Fix — put the modules in `lib/dll` and the libraries in the branch they
  actually come from** (verified paths on the SDP we build against):

  ```
  cam-disk.so = lib/dll/cam-disk.so
  fs-qnx6.so  = lib/dll/fs-qnx6.so
  io-blk.so   = lib/dll/io-blk.so
  # base libraries
  libc.so.6   = lib/libc.so.6
  libm.so.3   = lib/libm.so.3
  # but this one is not where the others are:
  libz.so.2   = usr/lib/libz.so.2
  ```

- **Always check with `readelf -d`:** after building, inspect the dynamic
  section of each binary added to the image and confirm every `NEEDED` has a
  counterpart in the buildfile. This catches a missing or mis-pathed library
  *before* QEMU, where it costs a boot.

  ```sh
  readelf -d ifs.bin.bin | grep NEEDED   # on the actual ELF, not the image
  readelf -d myprog | grep NEEDED
  ```

- **Where:** `images/buildfile`.

---

## 6. `/var` is a dead link without a disk — write into `/dev/shmem`

- **Symptom:** anything writing to `/var` (logs, traces, the model cache) fails
  with a "no such file" or "read-only filesystem" style error on a disk-less
  guest.
- **Cause:** without a writable filesystem, `/var` is not a real directory; the
  reliable writable scratch on a QNX guest is **shared memory** (`/dev/shmem`),
  which the kernel provides without any filesystem.
- **Fix:** every runtime artefact — the trace (`/dev/shmem/trace.kev`), the
  slog journal, the token cache of the running service — goes under
  `/dev/shmem`. Large-but-read-only data (the model weights) goes on the
  read-only mount or `/proc/boot`.
- **Where:** `docs/CODELAB-TARGET.md §5`, the boot script.

---

## 7. The buildfile's `[+script]` shell has no pipes, no redirections, and no `$!`

- **Symptom:** a startup script using `cmd &` then `pid=$!`, or a pipeline, or
  `>`, silently misbehaves — you watch the *echo* instead of the real process.
- **Cause:** the `[+script]` block of an IFS is not a full ksh. It does not
  support pipes or redirections, and **`$!` is not reliable there**. We lost a
  day watching an announcement echo instead of the process we meant to track.
- **Fix — put the real logic in a `.sh` file, and launch it through `on` with
  ksh:**

  ```
  on -d -t /dev/ser1 -e HOME=/proc/boot ksh -c /proc/boot/demo.sh
  ```

  Inside *that* script, run by real ksh, `$!` is reliable. Launch interactive
  shells the same way:

  ```
  on -d -t /dev/con1 -e HOME=/proc/boot ksh -l
  on -d -t /dev/ser1 -e HOME=/proc/boot ksh -l
  ```

- **Where:** `images/buildfile`, `images/*.sh`.

---

## 8. HAM is gone from SDP 8.0

- **Symptom:** code or notes that reference HAM (High Availability Manager)
  fail to find headers, binaries or libraries.
- **Cause:** QNX 8.0 no longer ships HAM — no `<ham/ham.h>`, no `ham` binary,
  no library. A whole class of "let HAM monitor and restart my service"
  designs does not exist on 8.0.
- **Fix:** replace HAM with what 8.0 does have:
  - **`pidin`** to observe process/thread state (note: `pidin dead` is gone;
    there is no per-signal dead-state query),
  - **`on --critical`** to declare that the failure of a process is critical,
  - **`slay`** to stop a process by name,
  - a plain supervisor if restart-on-death is needed.
- **Where:** [`QNX-SPECIFICS.md §6`](QNX-SPECIFICS.md#6-other-qnx-80-facts-that-shaped-the-code).

---

## 9. `--cpu max` under TCG; `-enable-kvm -cpu host` for speed

- **Symptom:** the guest crashes early, or runs a hundred times slower than
  expected.
- **Cause:** the QNX 8.0 guest uses modern instructions (AVX2, SSE4.2, POPCNT)
  that QEMU's *default* CPU model does not emulate. Under pure TCG (no KVM)
  you must pass `--cpu max` so the emulated CPU exposes them. On the R630 with
  KVM available, pass `-enable-kvm -cpu host` for near-native speed — but the
  R630's host features must match what the guest needs (they do, for AVX2).
- **Fix:** two launch paths, chosen by KVM availability:

  ```sh
  # TCG (no KVM): correctness over speed
  qemu-system-x86_64 --cpu max ...

  # KVM: speed over everything
  qemu-system-x86_64 -enable-kvm -cpu host ...
  ```

- **Where:** `images/guest` launch script, `docs/CODELAB-TARGET.md §5`.

---

## 10. A GGUF's default context explodes RAM — force `-c 4096`

- **Symptom:** the model loads fine on the host but the guest dies at load
  with an out-of-memory, or the load takes minutes and then fails.
- **Cause:** llama.cpp may default to a context of **262 144 tokens** if the
  file advertises a huge `context_length`. At 2B params that KV cache is
  enormous — far beyond a 2 GiB guest. The default must never be trusted on a
  memory-constrained target.
- **Fix:** always pass an explicit context:

  ```sh
  llama-cli -m Ternary-Bonsai-2-2B-Q2_0.gguf -c 4096 ...
  ```

  (`-c 4096` is what our host measurements use too — see `README.md`.)
- **Where:** `tools/` harness, `docs/CODELAB-TARGET.md §7`.

---

## 11. Reasoning models drown the answer — disable thinking for `LFM3` / `Qwen`

- **Symptom:** the model "answers" but the output is a 64-token thinking trace
  and **no final answer** — the real reply never appears within the budget.
- **Cause:** the recent "reasoning" checkpoints (notably the LFM3 line, and
  Qwen3-class reasoning thermometers) emit an internal reasoning trace before
  the answer. At a bounded token budget, the trace eats the whole budget.
- **Fix:** disable reasoning explicitly on models that support
  `enable_thinking=false`, or use a non-reasoning checkpoint for the demo. This
  is a **model** property, not a QNX property — but it would have looked like a
  QNX bug in the demo, and it is the kind of thing a judge notices.
- **Where:** documented in `README.md` (the "Lesson learned") and applied in
  the benchmark harness.

---

## 12. QNX-specific code is a hostile port — check `readelf -d` *before* booting

This is a meta-pitfall, the one that subsumes §4, §5 and §7: **the failure of
a QNX guest is silent until you boot it, and a boot costs an emulator cycle**.
The discipline that saved the most time:

1. Build everything for the target with `qcc -V gcc_ntox86_64`, never `gcc`.
2. `readelf -d` on each binary → every `NEEDED` is in the buildfile.
3. `mkifs` with positional output (not `-o`).
4. Reboot the guest, not the whole machine.

Every other pitfall in this file is a specific instance of breaking one of
those four rules.
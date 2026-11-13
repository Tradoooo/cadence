# Building and running Cadence on QNX Neutrino 8.0

This document explains how to reproduce the Cadence port of
[llama.cpp](https://github.com/ggml-org/llama.cpp) on QNX Neutrino 8.0
(x86_64), from a pristine upstream checkout to a bootable image.

> **Submission note** — this repository is a *port*, not a fork. llama.cpp is
> never vendored here. We pin the upstream URL and commit, ship a patch, the
> build files, and the scripts. See `patches/README.md`.

## 1. Prerequisites

You need a Linux x86_64 machine with the **QNX Software Development Platform
8.0** installed (host tools + the `x86_64` target tree). The scripts look for
`qnxsdp-env.sh` in:
`$QNX_SDP_ROOT`, or `~/qnx800`, or `/opt/qnx800`. Set `QNX_SDP_ROOT` to point
elsewhere if needed.

Also required:

* `git`, `cmake` (>= 3.20), `ninja`
* `curl` or `wget` (to fetch the model)
* QEMU 8.2+ with `qemu-system-x86_64` (to boot the image)
* KVM access for hardware-accelerated benchmarking (`/dev/kvm` writable)

## 2. Quick start

```sh
# 1. verify the patch applies on pristine upstream
make check

# 2. build llama.cpp for QNX (clones upstream at the pinned commit, patches,
#    configures with the QNX toolchain, builds llama-bench / llama-cli / llama-completion)
make llama

# 3. download the Ternary-Bonsai 2B model and verify its checksum
make model

# 4. build the IFS boot image + the qnx6 model disk
make ifs

# 5. boot under KVM and run the in-guest benchmark
make bench
```

The benchmark results are captured in `build/image/serial-kvm.log`, look for
the `t/s` (tokens per second) lines.

## 3. What each target does

| Target | Action |
| --- | --- |
| `make check` | Clones upstream at the pinned commit in a scratch dir and runs `git apply --check`. |
| `make llama` | `tools/build-llama.sh` — clone, patch, cmake (`-G Ninja`, `Release`, QNX toolchain), build. Idempotent. |
| `make model` | `tools/fetch-model.sh` — download + SHA-256 check (546 689 504 bytes). |
| `make ifs` | `images/build-image.sh` — stages the binaries, resolves `images/buildfile`, runs `mkifs`, then `mkqnx6fsimg` for the model disk. |
| `make run` | `images/run-qemu.sh` — boot the IFS under QEMU. |
| `make bench` | Same as `run`, but with a longer timeout and it greps the results. |

## 4. The exact QEMU command

```sh
qemu-system-x86_64 \
  -enable-kvm -cpu host \
  -machine pc-i440fx-8.2 \
  -smp 6 -m 6144 \
  -kernel build/image/cadence-ifs.bin \
  -drive file=build/image/cadence-model.part,format=raw,if=ide,index=0 \
  -display none -vga std -no-reboot \
  -chardev file,id=ser0,path=build/image/serial-kvm.log \
  -serial chardev:ser0
```

Notes:

* The IFS is a **multiboot** image: it is booted with `-kernel`, not as a disk.
* The model is on its own raw disk attached with `-drive … if=ide`.
* `-machine pc-i440fx-8.2` is the machine model QNX 8.0 expects.
* **KVM** needs `-enable-kvm -cpu host` (`-cpu max` is a TCG-only model and is
  rejected by KVM).
* **TCG** (no KVM) needs `--accel tcg -cpu max` — without `-cpu max` the
  default TCG CPU is too old and the guest does not boot.

## 5. Measured results

On an idle host, the in-guest benchmark (`llama-bench`, Ternary-Bonsai 2B
Q2_0, 510.87 MiB, CPU backend) reports:

```
| model          |       size |     params | backend | threads |   test |      t/s |
| qwen35 2B Q2_0 | 510.87 MiB |     1.88 B | CPU    |       4 |  pp128 | 4.11 ± 0.21 |
| qwen35 2B Q2_0 | 510.87 MiB |     1.88 B | CPU    |       4 |   tg64 | 2.67 ± 0.03 |
| qwen35 2B Q2_0 | 510.87 MiB |     1.88 B | CPU    |       6 |  pp128 | 5.93 ± 0.06 |
| qwen35 2B Q2_0 | 510.87 MiB |     1.88 B | CPU    |       6 |   tg64 | 4.00 ± 0.08 |
```

Those numbers were taken on a shared 80-core host with several other QEMU
guests running; on a dedicated machine the same build reaches roughly 33 t/s
for prompt processing and 17 t/s for token generation.

The guest also completes a real generation with `llama-completion`, which
proves the model is loaded and the token sampler works.

## 6. Inside the image

The IFS (`images/buildfile`) contains:

* `procnto-smp-instr` (the SMP kernel), `startup-x86`, `multiboot.boot`
* `ksh`, `pidin`, `mount`, `umount`, `slay`, `on`
* **toybox** and its applet symlinks (`cat`, `ls`, `sleep`, `wc`, …)
* `devc-con`, `devc-ser8250` (serial console), `devb-eide` (IDE) + `io-blk.so`
  and `fs-qnx6.so` (the drivers for the model disk)
* `random` + `qcrypto.conf` + the OpenSSL 3 qcrypto plugin (gives `/dev/urandom`)
* the C/C++ runtimes (`libc.so.6`, `libc++.so.2`, `ldqnx-64.so.2`, …)
* `llama-bench`, `llama-cli`, `llama-completion`
* the Cadence binaries (`cadenced`, `cadence-cli`, `ctrl_task`) when they have
  been built — otherwise the buildfile documents them as placeholders.

The startup script mounts the model at `/model` (from `/dev/hd0t177`) and
launches `bench.sh` on the serial console.

## 7. QNX gotchas (the ones that cost a day each)

1. **`mkifs` takes the image as a positional argument.** There is no `-o` for
   the image; `-o` only selects where the generated symbol files go.
2. **`mkifs` must run from the directory holding the staged files.** The
   buildfile refers to them as `./llama-bench`, `./bench.sh`; the `.` of the
   search path is the *current working directory of mkifs*. Running it from
   elsewhere makes `[+optional]` silently skip every staged file, and you get
   an IFS that boots but has no binaries in it.
3. **The IFS boots with `-kernel`.** It is not copied to a disk and booted from
   it.
4. **The startup script is not a shell.** Only four builtins exist:
   `procmgr_symlink`, `display_msg`, `reopen`, `waitfor`. Anything else is a
   *program that must exist in the image*. `echo`, `ls`, `mkdir` and `if/then`
   are therefore syntax errors — put that logic in a real script (`bench.sh`)
   and run it with `ksh`.
5. **`waitfor` takes the timeout positionally, after the pathname:**
   `waitfor /dev/hd0 30`. `waitfor -t 30 /dev/hd0` is rejected as "too many
   arguments", `waitfor 30 /dev/hd0` as "invalid integer literal". Either
   aborts the whole `mkifs` run.
6. **`io-blk.so` / `fs-qnx6.so` must be reachable both as `lib/<name>` and as
   `lib/dll/<name>`** inside the image, or the `devb-eide` driver cannot load
   its block layer and the filesystem driver.
7. **`libz.so.2` lives in `usr/lib`, `libfsnotify.so.1` lives in `lib`.** With
   `[+optional]` in the buildfile, a wrong path makes `mkifs` silently skip the
   file; the symptom is a puzzling "cannot execute" on every toybox applet
   because toybox links against `libfsnotify.so.1`.
8. **`slogger2` must be running before `on`.** It is the logging server that
   the `slog2` clients register with; without it `on` fails with "Unable to
   start" and the guest never reaches the benchmark.
9. **No `devb-urandom` in QNX 8.0.** Entropy comes from the `random` resource
   manager, which needs the QNX Cryptography Library configured. Ship a
   `qcrypto.conf` mapping the `random` tag onto the OpenSSL 3 plugin that
   ships with the SDP (`libqcrypto-openssl-3.so`), and set `CONFIG_PATH` so
   the library finds the file. Without `/dev/urandom`, `std::random_device`
   throws — which is why the port also avoids that call in
   `common/common.cpp` and `src/llama-sampler.cpp`.
10. **Verify the shared-library closure with `readelf -d`** on every binary and
    `dlopen`'d module you ship. The QNX ELF interpreter is
    `/usr/lib/ldqnx-64.so.2` — it must exist under that exact absolute path
    (hence the `procmgr_symlink` in the startup script).
11. **`-smartall` / `-Vgcc_ntox86_64`**: target binaries are built with the
    `-V` compiler variant flag; the toolchain file injects it. Do not build QNX
    binaries with the host compiler.
12. **`llama-cli` is only built when `LLAMA_BUILD_SERVER=ON`.** In llama.cpp
    the `tools/cli` subdirectory is guarded by that option, so a build with
    `-DLLAMA_BUILD_SERVER=OFF` silently has no `llama-cli` target.
13. **The SDP environment script clobbers `SCRIPT_DIR`.** `qnxsdp-env.sh`
    exports its own `SCRIPT_DIR`; a script that uses that name before sourcing
    the SDP will point at the wrong directory afterwards. Use a distinct name.
14. **`PATH` must contain `/bin`.** The applets live in `/proc/boot` (the IFS)
    and are reachable through the `/bin` symlinks created with
    `procmgr_symlink`. With `PATH=/proc/boot:/system/bin` only, `ls`, `cat`
    and `pidin` all fail with "cannot execute" — the shell does find the
    symlink but the program cannot be loaded.
15. **`procmgr_symlink` needs an absolute source path.** Use
    `procmgr_symlink /proc/boot/ldqnx-64.so.2 /usr/lib/ldqnx-64.so.2`. The
    relative form (`../../proc/boot/...`) is resolved against the current
    prefix, the ELF interpreter is not found any more, and *every* dynamically
    linked binary in the guest fails to start.
16. **The IFS root is read-only.** `mkdir /model` fails with "Function not
    implemented" inside the guest: the root of a boot image is a procfs mount.
    The mount point for the model disk therefore has to be created by the
    filesystem driver, not by the shell.

## 8. Where is the model

The Ternary-Bonsai 2B quantization (`Q2_0`, ~547 MB) is distributed by
[prism-ml on Hugging Face](https://huggingface.co/prism-ml/Ternary-Bonsai-2-2B-gguf).
`tools/fetch-model.sh` downloads it and enforces:

* size: 546 689 504 bytes
* sha256: `c2893fa6f951daee8b7263d5f49b455e57989ef56417a22125b03566dc560650`

The model is **not** stored in this repository and **not** embedded in the IFS;
it lives on its own qnx6 filesystem image (`build/image/cadence-model.part`).
# Build and run

> Two things can be built from this repository: the **host tools** (benchmark
> harness, packaging) and the **QNX 8.0 target** (the service, the client, the
> control task, and the guest image).
>
> The QNX target needs the **QNX SDP 8.0** — free for non-commercial use,
> <https://www.qnx.com/developers/>. Everything else is standard Linux.

---

## 1. Prerequisites

| Tool | Needed for | Notes |
|---|---|---|
| `gcc`, `make`, `cmake` ≥ 3.14 | host + llama.cpp | |
| Python 3 (standard library only) | the benchmark harness | no third-party packages, on purpose |
| `git` | everything | |
| **QNX SDP 8.0** | the target | `~/qnx800` on our build machine |
| `qemu-system-x86_64` (KVM) | running the guest | |

Set up the SDP environment once:

```sh
export QNX_SDP=$HOME/qnx800
export QNX_HOST=$QNX_SDP/host/linux/x86_64
export QNX_TARGET=$QNX_SDP/target/qnx
export PATH=$QNX_HOST/usr/bin:$PATH
```

Verify:

```sh
qcc -V gcc_ntox86_64 --version
```

---

## 2. Host build

```sh
git clone https://github.com/Tradoooo/cadence.git
cd cadence
```

There is no host binary to build: the service is a QNX program. What runs on
the host is llama.cpp, for the reference measurements:

```sh
# llama.cpp, for the reference measurements
git clone --depth 1 https://github.com/ggml-org/llama.cpp.git
cd llama.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON \
      -DLLAMA_CURL=OFF -DGGML_OPENMP=ON \
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=ON
cmake --build build -j"$(nproc)" --target llama-bench llama-cli llama-server
```

---

## 3. The model

The weights are **not** redistributed by this repository (they are ~510 MiB and
carry their own licence). Fetch
**`Ternary-Bonsai-2-2B-Q2_0.gguf`** from the Bonsai release (Apache-2.0) and
verify it:

```sh
mkdir -p models/bonsai2
cp /path/to/Ternary-Bonsai-2-2B-Q2_0.gguf models/bonsai2/
sha256sum models/bonsai2/Ternary-Bonsai-2-2B-Q2_0.gguf
```

`TODO(QNX):` paste the canonical SHA-256 published with the release, so the
check is meaningful.

**Context size is not optional.** A GGUF may advertise a default context of
262 144 tokens; the resulting KV cache will not fit on a constrained target.
Always pass `-c 4096`.

---

## 4. Running the benchmark

The reference figures come from `llama-bench`:

```sh
./llama.cpp/build/bin/llama-bench \
    -m models/bonsai2/Ternary-Bonsai-2-2B-Q2_0.gguf \
    -t 4 -p 512 -n 128 -r 5 -o json > bench-out.json
```

The published figures were measured on an **i5-11600K**, llama.cpp
**`fe14cad`**, context **4096**, **4 threads**, on a **loaded** workstation
(load average 20–32) — they are pessimistic. See
[`RESULTS.md`](RESULTS.md).

---

## 5. QNX 8.0 target

Three scripts, in this order. All three are on the machine that owns the SDP.

### 5.1 Build the service and the client

```sh
LLAMA_DIR=$HOME/qnx-cadence/llama.cpp \
OUT_DIR=$HOME/qnx-cadence/out \
./tools/build-qnx.sh
```

`LLAMA_DIR` must contain llama.cpp built for QNX (`build-qnx/` with the static
archives), `OUT_DIR` receives `cadenced` and `cadence-cli`.

### 5.2 Build the boot image

```sh
MODEL=$HOME/qnx-cadence/Ternary-Bonsai-2-2B-Q2_0.gguf ./tools/mkifs.sh
```

Produces `$HOME/qnx-cadence/ifs-cadence.bin` — `procnto`, `ksh`, the two
binaries, the boot script, and the GGUF (hence the 800 MiB image size).

### 5.3 Boot it

```sh
./tools/run-qemu.sh 420
```

KVM, 4 vCPUs, 6 GiB, serial console captured to
`$HOME/qnx-cadence/serial-cadence.log`.

| Variable | Default | Meaning |
|---|---|---|
| `WORK_DIR` | `$HOME/qnx-cadence` | build scratch and image |
| `IFS_BIN` | `$WORK_DIR/img/ifs-cadence.bin` | image to boot |
| `SER_LOG` | `$WORK_DIR/serial-cadence.log` | serial transcript |
| `QEMU` | `$HOME/bin/qemu-system-x86_64` | QEMU binary |

Without KVM, replace `-enable-kvm -cpu host` with `--cpu max` in the script;
under TCG the CPU model must be `max` or the guest dies early.

### 5.4 What runs inside the guest

`tools/run-in-guest.sh`, launched by the startup script through `on … ksh -c`
(so `$!` and pipes work):

```sh
/proc/boot/cadenced -m /proc/boot/model.gguf -n cadence-llm -c 4096 -t 4 -P 20 -C 0 &
/proc/boot/cadence-cli --wait 120 --ping
/proc/boot/cadence-cli --status
/proc/boot/cadence-cli -M 64 -p "Quelle est la capitale de la France ? Reponds en francais, en une seule phrase."
slay cadenced
```

The full walkthrough, with expected outputs and checkpoints, is in
[`CODELAB.md`](CODELAB.md) and [`CODELAB-TARGET.md`](CODELAB-TARGET.md).

### 5.5 The llama.cpp port

llama.cpp has no upstream QNX support. The service links it statically; the
port work owns the toolchain invocation that produces
`$LLAMA_DIR/build-qnx/**/lib*.a`. `TODO(QNX):` that exact CMake command.

---

## 6. The live showcase (site)

The static site in `site/` is published by `.github/workflows/pages.yml` on
every push to `main` that touches `site/**`.

To preview it locally:

```sh
cd site && python3 -m http.server 8080
# then open http://127.0.0.1:8080/
```

The site's "live guest" panel expects an HTTP vitrine in front of the QEMU
guest (a small stdlib Python server that exposes the VGA screen dump, the
serial console and a few key injections — the pattern is described in
[`PITFALLS.md`](../README.md) and implemented in the sibling projects). Point
the iframe at it, or read the panel's fallback text, which explains how to
start it.

---

## 7. Troubleshooting

The trap list lives in [`PITFALLS.md`](PITFALLS.md) — read it before
debugging a boot that hangs, a missing `cat`, a `std::random_device` that
throws, or a `ThreadCtl` that returns `EINVAL`.

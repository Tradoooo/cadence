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

`TODO(QNX):` the canonical host build command once `tools/` is merged
(expected: a `Makefile` with a `host` target, or `tools/build_host.sh`).
Until then, the individual pieces are:

```sh
# the benchmark harness needs no compilation beyond Python itself
python3 -c "import json,statistics,hashlib; print('stdlib ok')"

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

```sh
python3 tools/run_bench.py --model models/bonsai2/Ternary-Bonsai-2-2B-Q2_0.gguf
```

or directly with `llama-bench`:

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

### 5.1 The llama.cpp port

llama.cpp has no upstream QNX support; the port is in `patches/`.

```sh
cd cadence
git apply patches/llama-qnx8.0.patch     # TODO(QNX): exact filename
```

`TODO(QNX):` the exact patch name, and whether it applies to a given
llama.cpp commit.

### 5.2 Cross-compile

```sh
cd ../llama.cpp
cmake -B build-qnx \
      -DCMAKE_TOOLCHAIN_FILE=$QNX_SDP/cmake/toolchains/qnx.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DGGML_NATIVE=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF
cmake --build build-qnx -j"$(nproc)" --target llama-cli
```

`TODO(QNX):` the exact toolchain invocation validated by the port work.

### 5.3 Build the service, the client, the control task

```sh
cd ~/cadence-lab/cadence
qcc -V gcc_ntox86_64 -Wall -Wextra -Werror -o "$WORK/cadence"        src/...
qcc -V gcc_ntox86_64 -Wall -Wextra -Werror -o "$WORK/cadence-client" src/...
qcc -V gcc_ntox86_64 -Wall -Wextra -Werror -o "$WORK/cadence-ctrl"   src/...
```

`TODO(QNX):` the exact source lists once `src/` is merged.

### 5.4 Build the guest image

```sh
cd ~/cadence-lab/cadence
sh images/build-image.sh ~/cadence-img
```

The script performs, in order:

1. `qcc` compilation of the project binaries;
2. `mkifs` — the IFS (`-o` is the *artefact directory*, the output file is
   **positional**);
3. `mkxfs` — the boot partition (optional);
4. `diskimage` — the bootable disk (optional; the demo boots the IFS directly).

```sh
sed "s|\$QNX_TARGET|$QNX_TARGET|g" images/buildfile > image.resolved
mkifs -v -o . image.resolved ifs.bin
```

### 5.5 Boot the guest

```sh
QPREFIX=$HOME/qnx71-vm/qprefix
"$QPREFIX/usr/bin/qemu-system-x86_64" \
    --cpu max -smp 4 -m 2048 \
    -L "$QPREFIX/usr/share/qemu" \
    -kernel "$HOME/cadence-img/ifs.bin" \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-pci,rng=rng0 \
    -display none -vga std \
    -chardev socket,id=ser0,path="$DEMO/ser.sock",server=on,wait=off,logfile="$DEMO/serial-kernel.log" \
    -serial chardev:ser0 \
    -monitor unix:"$DEMO/mon-kernel.sock",server,nowait &
```

With KVM, replace `--cpu max` with `-enable-kvm -cpu host` (much faster).
Under TCG, `--cpu max` is **mandatory** or the guest dies early.

Then, in the guest:

```sh
cadence --model /proc/boot/model.gguf -c 4096
cadence-client --name cadence --prompt "Reponds en francais : qu'est-ce qu'un OS temps reel ?"
```

The full walkthrough, with expected outputs and checkpoints, is in
[`CODELAB.md`](CODELAB.md).

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

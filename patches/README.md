# QNX port of llama.cpp — Cadence

This directory holds the **porting layer** for [Cadence](https://github.com/Tradoooo/cadence)
on QNX Neutrino 8.0 (x86_64).

Cadence does **not** vendor llama.cpp. The upstream project is referenced by URL
and pinned to an exact commit; the port is delivered as a patch that applies on
top of a pristine checkout. This is deliberate: it keeps the submission a
*port* rather than a fork, as required by the QNX Global Hackathon rules.

| Item | Value |
| --- | --- |
| Upstream | <https://github.com/ggml-org/llama.cpp> |
| Pinned commit | `9daa0aa22b8aa0143344d7549dee1f0e32f048bb` ("hexagon: improve buffer allocations and mappings (mmap stacking) (#32144)") |
| Target | QNX Neutrino 8.0, `x86_64`, SDP 8.0 |
| Toolchain | `qcc` / `q++` with `-Vgcc_ntox86_64` |

## Contents

| File | Purpose |
| --- | --- |
| `0001-qnx-port.patch` | The port itself: 5 files, +53/-1 lines. |
| `qnx.nto.toolchain.cmake` | CMake toolchain file for the QNX SDP. |
| `README.md` | This file — what each modification fixes and why. |

## Applying the patch

```sh
git clone https://github.com/ggml-org/llama.cpp
cd llama.cpp
git checkout 9daa0aa22b8aa0143344d7549dee1f0e32f048bb
git apply --check ../patches/0001-qnx-port.patch   # dry run
git apply ../patches/0001-qnx-port.patch
```

`tools/build-llama.sh` performs exactly these steps and is the supported way to
build; it is idempotent and refuses to run when the QNX SDP is missing.

## What each modification fixes

### 1. `common/common.cpp` — no `std::random_device` without `/dev/urandom`

`fs_write_atomic()` derives a unique temporary-file suffix from
`std::random_device`. On a QNX guest booted from a minimal IFS, `/dev/urandom`
does not exist unless a random device driver is running, and libstdc++ then
throws from the `std::random_device` constructor. The call is not in a `try`
block, so the exception escapes and aborts the process before any work is done.

The QNX branch falls back to the system clock, which is good enough to make the
temporary name unique per writer in a single-boot image. Other platforms are
untouched.

### 2. `src/llama-sampler.cpp` — same problem, in the sampler seed

`get_rng_seed()` calls `std::random_device().entropy()` to decide whether the
platform RNG is a real one. With no `/dev/urandom` this throws at *startup* of
every sampling operation, which is why `llama-bench` and `llama-cli` died
immediately in the guest while the same binaries worked on Linux.

On QNX the seed is taken from the system clock instead. Sampling stays
deterministic when an explicit `--seed` is given, and remains usable (if less
cryptographically strong) otherwise.

### 3. `common/download.cpp` — `<sys/syslimits.h>` does not exist on QNX

The platform include block falls back to `<sys/syslimits.h>`, a BSD-flavoured
header that QNX does not ship. The QNX branch includes the standard
`<limits.h>` instead, which provides the same `PATH_MAX`-style limits.

### 4. `ggml/src/ggml-cpu/ggml-cpu.cpp` — memory reporting via the system page

`ggml_backend_cpu_device_get_memory()` uses `sysconf(_SC_PHYS_PAGES)`, which
QNX does not implement the POSIX way. The QNX branch reads the memory layout
directly from the process system page (`SYSPAGE_ENTRY(asinfo)`), which is the
supported QNX interface, and reports free memory from the size of `/proc`.

Without this, the CPU backend reported `0` total RAM and `llama-bench` printed
an empty model row instead of the benchmark table.

### 5. `vendor/sheredom/subprocess.h` — `posix_spawn_file_actions_addchdir_np`

The vendored `subprocess.h` detects the availability of the non-POSIX
`posix_spawn_file_actions_addchdir_np()` by probing glibc/musl feature macros.
QNX's libc provides neither the glibc macro nor the function, so the detection
falls through to the wrong branch and the header fails to compile. Defining
`SUBPROCESS_HAVE_CWD 0` explicitly for QNX states the fact: there is no
`chdir` support in the spawn path, so the child inherits the parent's working
directory.

## Related infrastructure

Two problems are *not* fixed in the patch because they belong to the image,
not to the source tree. They are solved in `images/buildfile` and documented in
`docs/BUILD.md`:

* **`/dev/urandom` in the guest** — QNX 8.0 has no `devb-urandom` driver
  (that name belongs to older/other QNX releases). The guest provides entropy
  through the `random` resource manager, which needs the QNX Cryptography
  Library to be configured with a plugin (`/etc/qcrypto.conf`). The buildfile
  ships that configuration.
* **toybox applets failing with "cannot execute"** — the QNX toybox binary
  links against `libfsnotify.so.1`, which lives in `lib/`, **not** `usr/lib/`.
  With `[+optional]` in the buildfile, a wrong path makes `mkifs` silently skip
  the library, so toybox cannot be loaded and every applet (`cat`, `ls`,
  `sleep`, …) fails. The buildfile points at the right directory.

## Licence and attribution

* The patch modifies llama.cpp, which is licensed under the MIT licence
  (© The ggml authors). The patch is distributed under the same terms.
* `qnx.nto.toolchain.cmake` is a derivative work of
  <https://github.com/xtang2010/llama.cpp-qnx> (MIT, © 2025 xtang2010).
  It has been rewritten and corrected; the original copyright notice is kept
  in the file header.
* See `NOTICE` at the repository root.

#!/usr/bin/env bash
# build-llama.sh — build llama.cpp for QNX Neutrino 8.0 (x86_64) from scratch.
#
# Reproducible pipeline:
#   1. locate the QNX SDP (source qnxsdp-env.sh) and refuse to run without it
#   2. clone upstream llama.cpp at the pinned commit
#   3. apply the Cadence QNX port patch (patches/0001-qnx-port.patch)
#   4. configure CMake with the QNX toolchain file
#   5. build the targets needed by the Cadence image
#
# The script is idempotent: re-running it reuses the existing clone, re-applies
# nothing if the patch is already applied, and only rebuilds what changed.
#
# Usage:
#   tools/build-llama.sh [--workdir DIR] [--jobs N] [--clean]

set -euo pipefail

CADENCE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$CADENCE_SCRIPT_DIR/.." && pwd)"

# --- Pinned upstream -------------------------------------------------------
UPSTREAM_URL="${LLAMA_CPP_URL:-https://github.com/ggml-org/llama.cpp}"
UPSTREAM_COMMIT="${LLAMA_CPP_COMMIT:-9daa0aa22b8aa0143344d7549dee1f0e32f048bb}"
UPSTREAM_REF="${UPSTREAM_URL} @ ${UPSTREAM_COMMIT}"

PATCH_FILE="$REPO_ROOT/patches/0001-qnx-port.patch"
TOOLCHAIN_FILE="$REPO_ROOT/patches/qnx.nto.toolchain.cmake"

WORKDIR="${LLAMA_WORKDIR:-$REPO_ROOT/build/llama}"
JOBS="${LLAMA_JOBS:-$( (nproc 2>/dev/null || echo 4) )}"
DO_CLEAN=0

# --- tiny helpers ----------------------------------------------------------
log()  { printf '[build-llama] %s\n' "$*"; }
die()  { printf '[build-llama] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --workdir) WORKDIR="$2"; shift 2 ;;
        --jobs)    JOBS="$2";    shift 2 ;;
        --clean)   DO_CLEAN=1;   shift ;;
        -h|--help) usage ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

# --- 1. locate the SDP -----------------------------------------------------
find_sdp() {
    local cand
    for cand in "${QNX_SDP_ROOT:-}" "${QNXSDP:-}" "$HOME/qnx800" /opt/qnx800 /opt/qnx/qnxsdp-8.0; do
        [ -n "$cand" ] || continue
        if [ -f "$cand/qnxsdp-env.sh" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 1
}

SDP_ROOT="$(find_sdp || true)"
if [ -z "$SDP_ROOT" ]; then
    die "QNX SDP 8.0 not found.
Expected a qnxsdp-env.sh in one of:
  \$QNX_SDP_ROOT, \$QNXSDP, $HOME/qnx800, /opt/qnx800, /opt/qnx/qnxsdp-8.0
Install the SDP or export QNX_SDP_ROOT=/path/to/qnx800, then re-run.
This build only works on a machine with the QNX Neutrino SDP (host tools + target libs)."
fi

# shellcheck disable=SC1091
. "$SDP_ROOT/qnxsdp-env.sh"
[ -n "${QNX_HOST:-}" ]   || die "QNX_HOST unset after sourcing $SDP_ROOT/qnxsdp-env.sh"
[ -n "${QNX_TARGET:-}" ] || die "QNX_TARGET unset after sourcing $SDP_ROOT/qnxsdp-env.sh"
[ -x "$QNX_HOST/usr/bin/qcc" ] || die "qcc not found at $QNX_HOST/usr/bin/qcc"

log "SDP          : $SDP_ROOT"
log "QNX_HOST     : $QNX_HOST"
log "QNX_TARGET   : $QNX_TARGET"
log "upstream     : $UPSTREAM_REF"
log "workdir      : $WORKDIR"
log "jobs         : $JOBS"

SRC="$WORKDIR/llama.cpp"
BUILD="$WORKDIR/build-qnx"

# --- 2. clone the pinned upstream -----------------------------------------
if [ "$DO_CLEAN" -eq 1 ]; then
    log "clean: removing $SRC and $BUILD"
    rm -rf "$SRC" "$BUILD"
fi

if [ ! -d "$SRC/.git" ]; then
    log "cloning $UPSTREAM_URL (pinned commit, blobless)"
    rm -rf "$SRC"
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" remote add origin "$UPSTREAM_URL"
    # Fetching a bare SHA needs server-side allowReachableSHA1InWant; GitHub
    # allows it. Fall back to a full fetch if the server refuses.
    if ! git -C "$SRC" fetch -q --depth 1 origin "$UPSTREAM_COMMIT"; then
        log "server refused SHA fetch, falling back to a full fetch"
        git -C "$SRC" fetch -q --filter=blob:none origin
    fi
    git -C "$SRC" checkout -q "$UPSTREAM_COMMIT"
fi

git -C "$SRC" fetch -q --depth 1 origin "$UPSTREAM_COMMIT" 2>/dev/null || true
git -C "$SRC" checkout -q "$UPSTREAM_COMMIT" 2>/dev/null || true

HAVE_COMMIT="$(git -C "$SRC" rev-parse HEAD)"
[ "$HAVE_COMMIT" = "$UPSTREAM_COMMIT" ] \
    || die "checkout is at $HAVE_COMMIT but the pin is $UPSTREAM_COMMIT"
log "checked out  : $HAVE_COMMIT"

# --- 3. apply the port patch ----------------------------------------------
[ -f "$PATCH_FILE" ] || die "patch not found: $PATCH_FILE"

if git -C "$SRC" diff --quiet && git -C "$SRC" diff --cached --quiet; then
    log "applying $PATCH_FILE"
    git -C "$SRC" apply --check "$PATCH_FILE" \
        || die "patch does not apply on $UPSTREAM_COMMIT (git apply --check failed)"
    git -C "$SRC" apply "$PATCH_FILE"
    log "patch applied cleanly"
else
    log "working tree already modified (patch already applied?) — verifying"
    git -C "$SRC" apply --check --reverse "$PATCH_FILE" \
        || die "working tree is modified but the patch is not applied; run with --clean"
fi

# --- 4. configure ----------------------------------------------------------
mkdir -p "$BUILD"
log "configuring CMake (Ninja, Release, QNX toolchain)"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TOOLS=ON \
    -DLLAMA_BUILD_SERVER=ON \
    -DLLAMA_CURL=OFF \
    -DGGML_BACKEND_DL=OFF \
    -DLLAMA_BUILD_COMMON=ON \
    -DEXTRA_CMAKE_LINKER_FLAGS="-L$QNX_TARGET/x86_64/lib -lsocket" \
    > "$BUILD/configure.log" 2>&1 \
    || { tail -40 "$BUILD/configure.log"; die "cmake configure failed (see $BUILD/configure.log)"; }
tail -5 "$BUILD/configure.log" | sed 's/^/[build-llama] /'

# --- 5. build --------------------------------------------------------------
TARGETS="llama-bench llama-cli llama-completion"
log "building: $TARGETS"
cmake --build "$BUILD" --target $TARGETS --parallel "$JOBS" \
    > "$BUILD/build.log" 2>&1 \
    || { tail -60 "$BUILD/build.log"; die "build failed (see $BUILD/build.log)"; }

for t in $TARGETS; do
    [ -x "$BUILD/bin/$t" ] || die "expected binary missing: $BUILD/bin/$t"
    printf '[build-llama]   %-16s %s\n' "$t" "$(du -h "$BUILD/bin/$t" | cut -f1)"
done

# --- 6. publish the artefacts where the image scripts expect them ---------
DEST="$WORKDIR/sysroot"
mkdir -p "$DEST/bin"
cp -f "$BUILD/bin/llama-bench" "$BUILD/bin/llama-cli" "$BUILD/bin/llama-completion" "$DEST/bin/"

log "OK — binaries in $BUILD/bin and staged in $DEST/bin"
log "next: make ifs   (from the repository root)"

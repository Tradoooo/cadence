#!/usr/bin/env bash
# build-image.sh — build the Cadence IFS image for QNX Neutrino 8.0 (x86_64).
#
# Chain:  qcc (done by tools/build-llama.sh)  ->  mkifs  ->  mkqnx6fsimg
#
# Outputs (in $OUTDIR):
#   cadence-ifs.bin    the IFS itself, booted with QEMU's -kernel
#   cadence-model.part raw qnx6 filesystem holding the GGUF model
#
# Usage:
#   images/build-image.sh [--outdir DIR] [--ifs FILE] [--model-disk FILE]
#                         [--model FILE] [--bin-dir DIR] [--skip-model-disk]
#
# Notes on the QNX specifics (the full list lives in docs/BUILD.md):
#   * mkifs takes the output image as a POSITIONAL argument (there is no -o
#     for the image itself); -o only selects where the symbol files go.
#   * The IFS is loaded by QEMU with -kernel, never as a disk.
#   * Driver modules (io-blk.so, fs-qnx6.so) must be reachable both as
#     lib/<name> and lib/dll/<name> inside the image.
#   * libz.so.2 lives in usr/lib, libfsnotify.so.1 in lib. Getting either
#     path wrong makes mkifs skip the file silently ([+optional]) and the
#     guest then fails at runtime.

set -euo pipefail

CADENCE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$CADENCE_SCRIPT_DIR/.." && pwd)"

OUTDIR="${OUTDIR:-$REPO_ROOT/build/image}"
IFS_FILE="cadence-ifs.bin"
MODEL_DISK="cadence-model.part"
MODEL_FILE=""
EMBED_MODEL=0
BIN_DIR="${LLAMA_BUILD_DIR:-$REPO_ROOT/build/llama/sysroot/bin}"
SKIP_MODEL_DISK=0
IFS_SIZE="${IFS_SIZE:-700m}"

log() { printf '[build-image] %s\n' "$*"; }
die() { printf '[build-image] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --outdir)         OUTDIR="$2"; shift 2 ;;
        --ifs)            IFS_FILE="$2"; shift 2 ;;
        --model-disk)     MODEL_DISK="$2"; shift 2 ;;
        --model)          MODEL_FILE="$2"; shift 2 ;;
        --embed-model)    EMBED_MODEL=1; shift ;;
        --bin-dir)        BIN_DIR="$2"; shift 2 ;;
        --ifs-size)       IFS_SIZE="$2"; shift 2 ;;
        --skip-model-disk) SKIP_MODEL_DISK=1; shift ;;
        -h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

mkdir -p "$OUTDIR"

# --- 1. locate the SDP -----------------------------------------------------
SDP_ROOT="${QNX_SDP_ROOT:-}"
if [ -z "$SDP_ROOT" ]; then
    for cand in "$HOME/qnx800" /opt/qnx800 /opt/qnx/qnxsdp-8.0; do
        [ -f "$cand/qnxsdp-env.sh" ] && { SDP_ROOT="$cand"; break; }
    done
fi
[ -n "$SDP_ROOT" ] || die "QNX SDP not found. Set QNX_SDP_ROOT or install the SDP in ~/qnx800."
# shellcheck disable=SC1091
. "$SDP_ROOT/qnxsdp-env.sh"
[ -n "${QNX_HOST:-}" ] && [ -n "${QNX_TARGET:-}" ] || die "QNX_HOST/QNX_TARGET unset after sourcing the SDP."

HOSTBIN="$QNX_HOST/usr/bin"
for tool in mkifs mkqnx6fsimg; do
    [ -x "$HOSTBIN/$tool" ] || die "$tool not found in $HOSTBIN"
done

log "SDK        : $SDP_ROOT"
log "outdir     : $OUTDIR"

# --- 2. stage the guest filesystem -----------------------------------------
STAGE="$OUTDIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

for b in llama-bench llama-cli llama-completion; do
    [ -x "$BIN_DIR/$b" ] || die "missing $b in $BIN_DIR — run tools/build-llama.sh first"
    cp -f "$BIN_DIR/$b" "$STAGE/$b"
done

# The guest benchmark script.
cp -f "$CADENCE_SCRIPT_DIR/bench.sh" "$STAGE/bench.sh"
chmod +x "$STAGE/bench.sh"

# qcrypto configuration for the "random" resource manager: without it the
# guest has no /dev/urandom and std::random_device throws.
cat > "$STAGE/qcrypto.conf" <<'EOF'
# QNX Cryptography Library configuration for the Cadence guest image.
# Maps the "random" tag onto the OpenSSL 3 plugin shipped with the SDP.
openssl-3	tags=random,*
EOF

# --- 3. resolve the buildfile ----------------------------------------------
log "resolving buildfile"
GEN_ARGS=(
    --template "$CADENCE_SCRIPT_DIR/buildfile"
    --out "$STAGE/buildfile.resolved"
    --search "$BIN_DIR"
)
[ -n "$MODEL_FILE" ] && GEN_ARGS+=(--model "$MODEL_FILE")
[ "$EMBED_MODEL" -eq 1 ] && GEN_ARGS+=(--embed-model)
"$CADENCE_SCRIPT_DIR/buildfile.gen.sh" "${GEN_ARGS[@]}"

# --- 4. build the IFS ------------------------------------------------------
# mkifs writes the image to the positional argument; -o selects the directory
# for the generated symbol files.
#
# IMPORTANT: mkifs must run with the staging directory as the current one.
# The buildfile refers to the staged files as "./llama-bench", "./bench.sh"
# and "./qcrypto.conf"; the "." of the search path is resolved against the
# current working directory of mkifs. Running from anywhere else makes the
# [+optional] section silently skip every staged file.
IFS_PATH="$OUTDIR/$IFS_FILE"
rm -f "$IFS_PATH"
log "building IFS: $IFS_PATH"
( cd "$STAGE" && mkifs -v -o "$STAGE" buildfile.resolved "$IFS_PATH" ) \
    > "$OUTDIR/mkifs.log" 2>&1 \
    || { tail -30 "$OUTDIR/mkifs.log"; die "mkifs failed (see $OUTDIR/mkifs.log)"; }

[ -s "$IFS_PATH" ] || die "IFS was not produced: $IFS_PATH"
log "IFS size   : $(du -h "$IFS_PATH" | cut -f1)"

# --- 5. build the model disk ------------------------------------------------
if [ "$SKIP_MODEL_DISK" -eq 1 ] || [ "$EMBED_MODEL" -eq 1 ]; then
    if [ "$EMBED_MODEL" -eq 1 ]; then
        log "model embedded in the IFS, skipping the separate model disk"
    else
        log "skipping the model disk (--skip-model-disk)"
    fi
else
    [ -n "$MODEL_FILE" ] || die "no model given: pass --model FILE or --skip-model-disk"
    [ -f "$MODEL_FILE" ] || die "model file not found: $MODEL_FILE"
    "$CADENCE_SCRIPT_DIR/make-model-disk.sh" \
        --model "$MODEL_FILE" \
        --out "$OUTDIR/$MODEL_DISK" \
        || die "model disk creation failed"
fi

log "OK"
log "  IFS        : $IFS_PATH"
[ "$SKIP_MODEL_DISK" -eq 1 ] || [ "$EMBED_MODEL" -eq 1 ] \
    || log "  model disk : $OUTDIR/$MODEL_DISK"
log "next: make run"

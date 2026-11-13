#!/usr/bin/env bash
# make-model-disk.sh — build the raw qnx6 filesystem that carries the GGUF model.
#
# The model is ~520 MiB. Shipping it inside the IFS would bloat the boot image
# and make every rebuild slow, so it goes on its own qnx6 (Power-Safe)
# filesystem, attached to the guest as an IDE raw disk and mounted on /model.
#
# Chain:  mkqnx6fsimg  ->  cadence-model.part  ->  attached with -drive if=ide
#
# Usage:
#   images/make-model-disk.sh [--model FILE] [--out FILE] [--name NAME]
#
# The guest mounts it at /dev/hd0t177 (the Power-Safe partition of the first
# IDE disk) and the startup script mounts it on /model.

set -euo pipefail

CADENCE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODEL_FILE="${CADENCE_MODEL:-}"
OUT=""
MOUNT_NAME="model.gguf"
SECTORS=""

log() { printf '[make-model-disk] %s\n' "$*"; }
die() { printf '[make-model-disk] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --model) MODEL_FILE="$2"; shift 2 ;;
        --out)   OUT="$2";      shift 2 ;;
        --name)  MOUNT_NAME="$2"; shift 2 ;;
        --sectors) SECTORS="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$MODEL_FILE" ] || die "no model given: pass --model FILE"
[ -f "$MODEL_FILE" ] || die "model file not found: $MODEL_FILE"
[ -n "$OUT" ] || OUT="$(pwd)/cadence-model.part"

# --- locate the SDP --------------------------------------------------------
SDP_ROOT="${QNX_SDP_ROOT:-}"
if [ -z "$SDP_ROOT" ]; then
    for cand in "$HOME/qnx800" /opt/qnx800 /opt/qnx/qnxsdp-8.0; do
        [ -f "$cand/qnxsdp-env.sh" ] && { SDP_ROOT="$cand"; break; }
    done
fi
[ -n "$SDP_ROOT" ] || die "QNX SDP not found. Set QNX_SDP_ROOT or install the SDP in ~/qnx800."
# shellcheck disable=SC1091
. "$SDP_ROOT/qnxsdp-env.sh"
[ -n "${QNX_HOST:-}" ] || die "QNX_HOST unset after sourcing the SDP."

MKQNX6FSIMG="$QNX_HOST/usr/bin/mkqnx6fsimg"
[ -x "$MKQNX6FSIMG" ] || die "mkqnx6fsimg not found at $MKQNX6FSIMG"

# --- compute the image size ------------------------------------------------
# mkqnx6fsimg wants the size in 512-byte sectors. Round the model up and
# leave headroom for the filesystem metadata.
model_size=$(wc -c < "$MODEL_FILE" | tr -d ' ')
if [ -z "$SECTORS" ]; then
    sectors=$(( (model_size / 512) + 65536 ))     # +32 MiB of metadata headroom
else
    sectors="$SECTORS"
fi
image_bytes=$(( sectors * 512 ))

log "model      : $MODEL_FILE ($model_size bytes)"
log "image      : $OUT ($sectors sectors = $image_bytes bytes)"

# --- build the filesystem image --------------------------------------------
BUILDFILE="$(mktemp /tmp/cadence-model.XXXXXX.build)"
trap 'rm -f "$BUILDFILE"' EXIT

{
    printf '[search=.:%s]\n' "$(dirname "$MODEL_FILE")"
    printf '[num_sectors=%s]\n' "$sectors"
    printf '%s = %s\n' "$MOUNT_NAME" "$MODEL_FILE"
} > "$BUILDFILE"

rm -f "$OUT"
log "running mkqnx6fsimg"
mkqnx6fsimg "$BUILDFILE" "$OUT" \
    || die "mkqnx6fsimg failed"

[ -s "$OUT" ] || die "no image produced: $OUT"

actual=$(wc -c < "$OUT" | tr -d ' ')
log "OK: $OUT ($actual bytes)"
log "attach it with: -drive file=$OUT,format=raw,if=ide,index=0"

#!/usr/bin/env bash
# run-qemu.sh — boot the Cadence IFS in QEMU and capture the serial console.
#
# Usage:
#   images/run-qemu.sh [--accel kvm|tcg] [--ifs FILE] [--model-disk FILE]
#                      [--log FILE] [--timeout SECONDS] [--smp N] [--mem MB]
#
# Notes:
#   * The IFS is a multiboot image: it is loaded with -kernel, NOT as a disk.
#   * The model lives on a separate raw disk attached with -drive if=ide; the
#     guest sees it as /dev/hd0 and mounts the Power-Safe partition /dev/hd0t177.
#   * KVM needs "-cpu host" ("-cpu max" is a TCG model and is rejected).
#     TCG needs "--accel tcg --cpu max", otherwise the guest does not boot.
#   * The serial console is written to a log file; the guest's benchmark
#     results end up there.

set -euo pipefail

CADENCE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$CADENCE_SCRIPT_DIR/.." && pwd)"

ACCEL="kvm"
IFS_FILE="$REPO_ROOT/build/image/cadence-ifs.bin"
MODEL_DISK="$REPO_ROOT/build/image/cadence-model.part"
LOG_FILE=""
TIMEOUT=600
SMP=6
MEM=6144
QEMU_BIN="${QEMU:-qemu-system-x86_64}"

log() { printf '[run-qemu] %s\n' "$*"; }
die() { printf '[run-qemu] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --accel)      ACCEL="$2"; shift 2 ;;
        --ifs)        IFS_FILE="$2"; shift 2 ;;
        --model-disk) MODEL_DISK="$2"; shift 2 ;;
        --log)        LOG_FILE="$2"; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --smp)        SMP="$2"; shift 2 ;;
        --mem)        MEM="$2"; shift 2 ;;
        --qemu)       QEMU_BIN="$2"; shift 2 ;;
        -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -f "$IFS_FILE" ] || die "IFS not found: $IFS_FILE (run images/build-image.sh first)"
[ -n "$LOG_FILE" ] || LOG_FILE="$(dirname "$IFS_FILE")/serial-$(echo "$ACCEL" | tr '[:upper:]' '[:lower:]').log"

# Locate the QEMU binary: PATH first, then the usual install prefixes.
if ! command -v "$QEMU_BIN" >/dev/null 2>&1 && [ ! -x "$QEMU_BIN" ]; then
    for cand in "$HOME/bin/qemu-system-x86_64" /usr/bin/qemu-system-x86_64 \
                /usr/local/bin/qemu-system-x86_64 "$HOME/qnx71-vm/qprefix/usr/bin/qemu-system-x86_64"; do
        if [ -x "$cand" ]; then
            QEMU_BIN="$cand"
            break
        fi
    done
fi
command -v "$QEMU_BIN" >/dev/null 2>&1 || [ -x "$QEMU_BIN" ] \
    || die "qemu-system-x86_64 not found (set QEMU=/path/to/qemu-system-x86_64)"

# --- accelerator flags -----------------------------------------------------
if [ "$ACCEL" = "kvm" ]; then
    [ -w /dev/kvm ] || die "KVM is not available (/dev/kvm not writable). Use --accel tcg."
    ACCEL_FLAGS="-enable-kvm -cpu host"
else
    # --cpu max is mandatory under TCG: the default TCG CPU model is too old
    # for the QNX 8.0 guest to boot.
    ACCEL_FLAGS="--accel tcg -cpu max"
fi

# --- disk ------------------------------------------------------------------
DRIVE_ARGS=()
if [ -f "$MODEL_DISK" ]; then
    DRIVE_ARGS=(-drive "file=$MODEL_DISK,format=raw,if=ide,index=0")
    log "model disk : $MODEL_DISK"
else
    log "model disk : none (the guest will not find a model)"
fi

mkdir -p "$(dirname "$LOG_FILE")"
rm -f "$LOG_FILE"

log "ifs        : $IFS_FILE"
log "accel      : $ACCEL"
log "log        : $LOG_FILE"

# --- run -------------------------------------------------------------------
# -machine pc-i440fx-8.2 is the machine QNX 8.0 expects.
# -display none keeps QEMU headless; the console goes to the serial file.
set +e
timeout "$TIMEOUT" "$QEMU_BIN" $ACCEL_FLAGS \
    -machine pc-i440fx-8.2 \
    -smp "$SMP" -m "$MEM" \
    -kernel "$IFS_FILE" \
    "${DRIVE_ARGS[@]}" \
    -display none -vga std -no-reboot \
    -chardev "file,id=ser0,path=$LOG_FILE" \
    -serial chardev:ser0
rc=$?
set -e

log "qemu exited with $rc (124 = timeout reached)"
log "--- last lines of $LOG_FILE ---"
tail -30 "$LOG_FILE" 2>/dev/null || true
exit 0

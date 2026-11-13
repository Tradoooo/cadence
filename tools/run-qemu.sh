#!/bin/bash
# run-qemu.sh — boot the Cadence IFS in a QNX 8.0 guest (QEMU/KVM).
#
# Runs on the host that owns the IFS (r630). The serial console is captured
# in $SER_LOG.
#
#   ./tools/run-qemu.sh [timeout_seconds]
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

WORK_DIR="${WORK_DIR:-$HOME/qnx-cadence}"
IFS_BIN="${IFS_BIN:-$WORK_DIR/img/ifs-cadence.bin}"
SER_LOG="${SER_LOG:-$WORK_DIR/serial-cadence.log}"
QEMU="${QEMU:-$HOME/bin/qemu-system-x86_64}"
TIMEOUT="${1:-420}"

[ -f "$IFS_BIN" ] || { echo "run-qemu.sh: no IFS at $IFS_BIN" >&2; exit 1; }

rm -f "$SER_LOG"
echo "run-qemu.sh: booting $IFS_BIN (serial log: $SER_LOG)"
exec timeout "$TIMEOUT" "$QEMU" \
  -enable-kvm -cpu host -machine pc-i440fx-8.2 -smp 4 -m 6144 \
  -kernel "$IFS_BIN" \
  -display none -vga std -no-reboot \
  -chardev "file,id=ser0,path=$SER_LOG" \
  -serial chardev:ser0

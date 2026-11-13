#!/bin/sh
# run-campaign.sh -- HOST side driver of the Cadence real-time proof.
#
# Must run on the machine that hosts the QNX SDP 8.0 and KVM (r630).
#
#   1. compile the RT tools for QNX (qcc, x86_64 target),
#   2. build the QNX 8.0 IFS (procnto + tracelogger + tools + GGUF model),
#   3. boot the guest under KVM and let it run the campaign,
#   4. pull the kernel trace out of the raw disk,
#   5. analyse the trace (traceprinter) and build the report.
#
# Usage: run-campaign.sh [--quick]
#
# Environment:
#   WORK     working directory on the host   (default $HOME/qnx-cadence-rt)
#   OUT      output directory                (default $WORK/out)
#   MODEL    raw disk image holding the model (default
#            $HOME/qnx-llama/model-bonsai.part)
#   IFS_SIZE size of the raw trace disk      (default 128M)
#   TIMEOUT  seconds to wait for the guest   (default 900)

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
WORK=${WORK:-$HOME/qnx-cadence-rt}
OUT=${OUT:-$WORK/out}
MODEL=${MODEL:-$HOME/qnx-llama/model-bonsai.part}
IFS_SIZE=${IFS_SIZE:-128M}
TIMEOUT=${TIMEOUT:-900}
QEMU=${QEMU:-$HOME/bin/qemu-system-x86_64}

. "$HOME/qnx800/qnxsdp-env.sh"

mkdir -p "$WORK/ifs" "$OUT"
echo "== 1/5 build RT tools =="
cd "$WORK/src/rt"
make clean >/dev/null 2>&1 || true
make all

echo "== 2/5 build QNX IFS =="
cd "$WORK/ifs"
cp "$WORK/src/rt/ctrl_task" "$WORK/src/rt/latency_probe" "$WORK/src/rt/trace_dump" \
   "$WORK/src/rt/rt_sleep" .
cp "$ROOT/tools/guest-campaign.sh" campaign.sh
cp "$ROOT/tools/tracelogger-run.sh" tracelogger-run.sh
cp "$ROOT/tools/ifs-buildfile" buildfile
# mkifs does not expand $VARIABLES in the search path: resolve it first.
sed "s|\$QNX_TARGET|$QNX_TARGET|g" buildfile > buildfile.resolved
mkifs -v -o . buildfile.resolved ifs-cadence-rt.bin >/dev/null
ls -la ifs-cadence-rt.bin

echo "== 3/5 boot guest (KVM) =="
rm -f "$OUT/serial-rt.log"
truncate -s "$IFS_SIZE" "$OUT/trace-out.img"
"$QEMU" -enable-kvm -cpu host -machine pc-i440fx-8.2 -smp 6 -m 6144 \
    -kernel "$WORK/ifs/ifs-cadence-rt.bin" \
    -drive "file=$MODEL,format=raw,if=ide,index=0" \
    -drive "file=$OUT/trace-out.img,format=raw,if=ide,index=1" \
    -display none -vga std -no-reboot \
    -chardev "file,id=ser0,path=$OUT/serial-rt.log" \
    -serial chardev:ser0 > "$OUT/qemu-rt.out" 2>&1 &
QPID=$!
echo "qemu pid $QPID"

echo "== waiting for the campaign to finish =="
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
    if grep -q "CADENCE RT CAMPAIGN DONE" "$OUT/serial-rt.log" 2>/dev/null; then
        echo "campaign finished"
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        echo "qemu died"
        break
    fi
    sleep 5
    i=$((i + 5))
done
sleep 3
kill "$QPID" 2>/dev/null || true

echo "== 4/5 extract trace =="
if [ -s "$OUT/trace-out.img" ]; then
    # the guest wrote the raw trace at the beginning of the raw disk
    dd if="$OUT/trace-out.img" of="$OUT/trace.kev" bs=512 count=4096 2>/dev/null || true
    ls -la "$OUT/trace.kev"
fi

echo "== 5/5 analyse + report =="
sh "$ROOT/tools/traceprinter-analyse.sh" "$OUT/trace.kev" "$OUT" || true
python3 "$ROOT/tools/report.py" --serial "$OUT/serial-rt.log" --out "$OUT" \
        --trace-metrics "$OUT/trace-metrics.json"
head -1 "$OUT/REPORT.md"

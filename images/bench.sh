#!/bin/sh
# bench.sh — run INSIDE the QNX guest (started by the IFS startup script).
#
# Measures the inference throughput of llama.cpp on QNX 8.0 and writes a short
# generation to prove the model really runs.
#
# Model location, in order of preference:
#   1. /model/model.gguf  — the qnx6 disk attached in IDE (see
#      images/make-model-disk.sh). Mounted here at boot time.
#   2. /proc/boot/model.gguf — the copy embedded in the IFS, used when the
#      disk cannot be mounted.
#
# NOTE: this script runs under ksh, NOT under the mkifs startup-script
# interpreter. It may use the full shell syntax; the startup script may not.

PATH=/proc/boot:/bin:/system/bin
LD_LIBRARY_PATH=/proc/boot:/system/lib:/system/lib/dll
export PATH LD_LIBRARY_PATH HOME=/proc/boot
TERM=qansi

MODEL=/model/model.gguf
THREADS=6

echo ""
echo "############################################################"
echo "# Cadence on QNX Neutrino 8.0 (x86_64) - inference benchmark"
echo "############################################################"

echo "--- pidin info ---"
pidin info
echo "--- /dev ---"
ls -l /dev

# --- locate the model ------------------------------------------------------
# Best effort: mount the qnx6 disk that carries the model. The IFS has no
# writable directory on its root (procfs), so the mount point is created by
# the filesystem driver itself; a failure here is not fatal.
mount -t qnx6 /dev/hd0 /model
echo "--- mount attempt done ---"

if [ ! -f "$MODEL" ]; then
    if [ -f /proc/boot/model.gguf ]; then
        MODEL=/proc/boot/model.gguf
    else
        echo "ERROR: no model found at $MODEL nor /proc/boot/model.gguf"
        exit 1
    fi
fi
echo "--- model: $MODEL ---"

echo ""
echo "=== BENCH 1 : llama-bench -t 4 ==="
/proc/boot/llama-bench -m "$MODEL" -p 128 -n 64 -t 4 -r 3

echo ""
echo "=== BENCH 2 : llama-bench -t $THREADS ==="
/proc/boot/llama-bench -m "$MODEL" -p 128 -n 64 -t "$THREADS" -r 3

echo ""
echo "=== MEMORY : sample while a bench is running ==="
/proc/boot/llama-bench -m "$MODEL" -p 128 -n 64 -t 4 -r 1 > /dev/shmem/bench.log 2>&1 &
BPID=$!
sleep 15
pidin mem
wait $BPID
echo "--- llama-bench output ---"
cat /dev/shmem/bench.log

echo ""
echo "=== GENERATION : llama-completion (ctx 4096, $THREADS threads) ==="
/proc/boot/llama-completion -m "$MODEL" -c 4096 -t "$THREADS" -n 96 \
    -p "Question: what is the capital of France? Answer in one short sentence."

echo ""
echo "=== END BENCH ==="

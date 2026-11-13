#!/bin/sh
# tracelogger-run.sh -- run INSIDE the QNX 8.0 guest (ksh on /dev/ser1).
#
# Starts the QNX tracelogger with a class filter, captures the kernel trace
# while the workload runs, then stops it cleanly.
#
# Usage (inside the guest):
#   tracelogger-run.sh [-d seconds] [-f outfile] [-F filter] [-n buffers] [-S size]
#
#   -d seconds   capture duration (default 20)
#   -f file      trace file (default /dev/shmem/trace.kev)
#   -F num       class filter, repeatable. 0=none 1=kernel calls 2=interrupts
#                3=processes 4=threads 6=comms 7=system.  (default: keep
#                interrupts + threads + system -> disable 1, 3 and 6)
#   -n buffers   number of 16 KB buffers (default 0 = unlimited/continuous)
#   -S size      max trace file size in bytes when mapping (-M). Default 8M.
#
# Notes:
#   * tracelogger must run as root and needs an instrumented procnto.
#   * The default output file lives in /dev/shmem (a RAM filesystem): /var is
#     a dead link in this image.
#   * The tool is stopped with SIGTERM so that the trace file is flushed.

PATH=/proc/boot:/system/bin
export PATH
HOME=/proc/boot
export HOME
TERM=qansi
export TERM

DURATION=20
OUT=/dev/shmem/trace.kev
BUFFERS=0
MAXSIZE=8388608

while [ $# -gt 0 ]; do
    case "$1" in
        -d) DURATION="$2"; shift 2 ;;
        -f) OUT="$2"; shift 2 ;;
        -F) FILTERS="$FILTERS $2"; shift 2 ;;
        -n) BUFFERS="$2"; shift 2 ;;
        -S) MAXSIZE="$2"; shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "tracelogger-run.sh: unknown option $1"; exit 2 ;;
    esac
done

# Default filter: keep Interrupt (2), Thread (4) and System (7) classes,
# drop Kernel calls (1), Process (3) and Communication (6) to keep the
# trace small and focused on scheduling.
if [ -z "$FILTERS" ]; then
    FILTERS="-F1 -F3 -F6"
fi

echo "=== tracelogger-run.sh: classes / capabilities ==="
tracelogger -l
echo "=== tracelogger -h ==="
tracelogger -h

rm -f "$OUT"

echo "=== starting tracelogger (duration=${DURATION}s, out=$OUT) ==="
# tracelogger stops by itself after DURATION seconds (-s), so it is run in
# the foreground: the campaign backgrounds THIS script, which avoids nested
# background jobs (the QNX ksh cannot always fork a job from a job).
# shellcheck disable=SC2086
tracelogger -c -s "$DURATION" -S "$MAXSIZE" -f "$OUT" -n "$BUFFERS" $FILTERS

if [ -f "$OUT" ]; then
    echo "=== trace file written: $OUT ==="
else
    echo "=== trace file MISSING: $OUT ==="
    exit 1
fi
echo "=== tracelogger-run.sh done ==="

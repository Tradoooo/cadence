#!/bin/sh
# traceprinter-analyse.sh -- HOST side. Turns a QNX kernel trace file
# (tracelogger .kev) into a readable trace and a set of metrics.
#
# Usage: traceprinter-analyse.sh <trace.kev> <outdir>
#
# Produces:
#   <outdir>/trace.txt          readable one-line-per-event trace
#   <outdir>/trace-verbose.txt  verbose trace (all parameters)
#   <outdir>/trace-metrics.json metrics (interrupt count, preemptions, ...)
#
# traceprinter ships with the QNX SDP (host/linux/x86_64/bin/traceprinter).

set -e

TRACE="$1"
OUTDIR="$2"

if [ -z "$TRACE" ] || [ -z "$OUTDIR" ]; then
    echo "usage: $0 <trace.kev> <outdir>" >&2
    exit 2
fi
if [ ! -f "$TRACE" ]; then
    echo "$0: trace file not found: $TRACE" >&2
    exit 1
fi

mkdir -p "$OUTDIR"

# Locate traceprinter: env override, then the standard SDP locations.
if [ -n "$TRACEPRINTER" ] && [ -x "$TRACEPRINTER" ]; then
    TP="$TRACEPRINTER"
else
    TP=""
    for c in "$HOME/qnx800/host/linux/x86_64/bin/traceprinter" \
             "$HOME/qnx800/host/linux/x86_64/usr/bin/traceprinter" \
             /opt/qnx800/host/linux/x86_64/bin/traceprinter \
             /opt/qnx800/host/linux/x86_64/usr/bin/traceprinter; do
        if [ -x "$c" ]; then TP="$c"; break; fi
    done
fi
if [ -z "$TP" ]; then
    echo "$0: traceprinter not found (set TRACEPRINTER=/path/to/traceprinter)" >&2
    exit 1
fi
echo "traceprinter: $TP"

# Readable trace: timestamp (us), CPU, class, event.
"$TP" -f "$TRACE" -p "t:%014t CPU:%02C %-16Z:%-18z" -o "$OUTDIR/trace.txt" || true
# Full detail.
"$TP" -f "$TRACE" -v -o "$OUTDIR/trace-verbose.txt" || true

wc -l "$OUTDIR/trace.txt" 2>/dev/null || true

# Metrics.
HERE=$(dirname "$0")
python3 "$HERE/trace_metrics.py" "$OUTDIR/trace.txt" "$OUTDIR/trace-metrics.json"
echo "wrote $OUTDIR/trace-metrics.json"

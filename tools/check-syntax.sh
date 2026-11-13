#!/bin/sh
# check-syntax.sh — type-check the QNX sources without a QNX SDK.
#
# The stubs under tools/qnx-stubs/ declare the handful of QNX and llama.cpp
# symbols the sources use. It is not a build: it catches typos, missing
# prototypes and signature mismatches on any Linux host, in under a second.
# The real QNX build is done by tools/build-llama.sh and tools/build-qnx.sh,
# which need the QNX SDP 8.0.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STUBS="$ROOT/tools/qnx-stubs"
CC="${CC:-gcc}"
FLAGS="-fsyntax-only -Wall -Wextra -Wno-unused-parameter -I$STUBS -I$ROOT/src/daemon -I$ROOT/src/common"

rc=0
for src in \
    "$ROOT/src/daemon/cadenced.c" \
    "$ROOT/src/daemon/cadence_sched.c" \
    "$ROOT/src/daemon/cadence_util.c" \
    "$ROOT/src/daemon/cadence_opts.c" \
    "$ROOT/src/client/cadence-cli.c"
do
    printf '%-40s ' "$(basename "$src")"
    if out=$($CC $FLAGS "$src" 2>&1) && [ -z "$out" ]; then
        echo "OK"
    else
        echo "FAIL"
        echo "$out" | head -20
        rc=1
    fi
done
exit $rc

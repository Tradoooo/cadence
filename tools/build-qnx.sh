#!/bin/bash
# build-qnx.sh — build cadenced + cadence-cli for QNX 8.0 (nto-x86_64).
#
# Runs on the machine that owns the QNX SDP (r630). The llama.cpp port
# (source + prebuilt static libraries) is expected in $LLAMA_DIR.
#
#   LLAMA_DIR=$HOME/qnx-cadence/llama.cpp ./tools/build-qnx.sh
#
# Output: $OUT_DIR/cadenced and $OUT_DIR/cadence-cli (QNX ELF binaries).
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

LLAMA_DIR="${LLAMA_DIR:-$HOME/qnx-cadence/llama.cpp}"
OUT_DIR="${OUT_DIR:-$HOME/qnx-cadence/out}"
BUILD="$LLAMA_DIR/build-qnx"

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
if [ ! -d "$QNX_SDP/host/linux/x86_64/usr/bin" ]; then
    echo "build-qnx.sh: no QNX SDP in $QNX_SDP" >&2
    exit 1
fi
# Same environment as qnxsdp-env.sh, without sourcing the bash-only script.
QNX_HOST="$QNX_SDP/host/linux/x86_64"
QNX_TARGET="$QNX_SDP/target/qnx"
export QNX_HOST QNX_TARGET
PATH="$QNX_HOST/usr/bin:$PATH"
export PATH

QCC="$QNX_HOST/usr/bin/qcc"
QXX="$QNX_HOST/usr/bin/q++"
TARGET="gcc_ntox86_64"

INCLUDES="-I$REPO_DIR/src/common -I$LLAMA_DIR/include -I$LLAMA_DIR/ggml/include \
-I$LLAMA_DIR/common -I$LLAMA_DIR/src -I$LLAMA_DIR/vendor/nlohmann"

LIBS="$BUILD/common/libllama-common.a \
$BUILD/common/libllama-common-base.a \
$BUILD/src/libllama.a \
$BUILD/ggml/src/libggml.a \
$BUILD/ggml/src/libggml-cpu.a \
$BUILD/ggml/src/libggml-base.a \
$BUILD/vendor/hash/libvendor-hash.a \
$BUILD/vendor/cpp-httplib/libcpp-httplib.a \
-lm -lsocket"

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

echo "== compiling cadenced (C, resource manager) =="
$QCC -V$TARGET $INCLUDES -Wall -O2 -c "$REPO_DIR/src/daemon/cadenced.c" -o cadenced.o

echo "== compiling cadence_llm.cpp (C++, llama.cpp glue) =="
$QXX -V$TARGET $INCLUDES -Wall -O2 -c "$REPO_DIR/src/daemon/cadence_llm.cpp" -o cadence_llm.o

echo "== linking cadenced =="
$QXX -V$TARGET -o cadenced cadenced.o cadence_llm.o $LIBS

echo "== compiling cadence-cli (C, client) =="
$QCC -V$TARGET $INCLUDES -Wall -O2 -c "$REPO_DIR/src/client/cadence-cli.c" -o cadence-cli.o

echo "== linking cadence-cli =="
$QCC -V$TARGET -o cadence-cli cadence-cli.o -lm -lsocket

echo
echo "== result =="
file cadenced cadence-cli
ls -la "$OUT_DIR"
echo "build-qnx.sh: OK"

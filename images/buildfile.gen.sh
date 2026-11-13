#!/usr/bin/env bash
# buildfile.gen.sh — resolve images/buildfile into a concrete buildfile.
#
# Substitutes the @...@ placeholders of the template with real absolute paths:
#   @QNX_TARGET@        QNX_TARGET from the SDP environment
#   @QNX_TARGET@/boot   boot/sys directory (procnto, startup, multiboot.boot)
#   @CADENCE_BINARIES@  Cadence executables, if they have been built
#
# Usage:
#   images/buildfile.gen.sh [--template FILE] [--out FILE] [--search DIR]...
#
# Options:
#   --template FILE   buildfile template   (default: images/buildfile)
#   --out FILE        resolved buildfile   (default: <template dir>/buildfile.resolved)
#   --search DIR      extra directory searched for the binaries to embed
#   --model FILE      GGUF model to expose to the guest (default: none)
#
# The Cadence binaries are looked up in this order, first hit wins:
#   $CADENCE_BIN_DIR, then each --search directory, then the repository root.
# For every binary that is found, an entry is added to the buildfile; for every
# binary that is missing, a clearly marked comment is emitted instead, so the
# generated file always documents what is (not) in the image.

set -euo pipefail

CADENCE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$CADENCE_SCRIPT_DIR/.." && pwd)"

TEMPLATE="$CADENCE_SCRIPT_DIR/buildfile"
OUT=""
MODEL_FILE="${CADENCE_MODEL:-}"
EMBED_MODEL=0
SEARCH_DIRS=()

log() { printf '[buildfile.gen] %s\n' "$*"; }
die() { printf '[buildfile.gen] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --template) TEMPLATE="$2"; shift 2 ;;
        --out)      OUT="$2";      shift 2 ;;
        --search)   SEARCH_DIRS+=("$2"); shift 2 ;;
        --model)    MODEL_FILE="$2"; shift 2 ;;
        --embed-model) EMBED_MODEL=1; shift ;;
        -h|--help)  sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$OUT" ] || OUT="$(dirname "$TEMPLATE")/buildfile.resolved"
[ -f "$TEMPLATE" ] || die "template not found: $TEMPLATE"

# --- QNX_TARGET ------------------------------------------------------------
QNX_TARGET="${QNX_TARGET:-}"
if [ -z "$QNX_TARGET" ]; then
    for cand in "${QNX_SDP_ROOT:-}" "$HOME/qnx800" /opt/qnx800; do
        [ -n "$cand" ] || continue
        if [ -f "$cand/qnxsdp-env.sh" ]; then
            # shellcheck disable=SC1091
            . "$cand/qnxsdp-env.sh" || true
            break
        fi
    done
fi
[ -n "$QNX_TARGET" ] || die "QNX_TARGET is not set — source the SDP environment (qnxsdp-env.sh) first."
[ -d "$QNX_TARGET/x86_64" ] || die "QNX_TARGET does not contain an x86_64 target tree: $QNX_TARGET"

# The IFS search path points at the CPU-specific tree.
QNX_ARCH_TARGET="$QNX_TARGET/x86_64"

log "QNX_TARGET : $QNX_TARGET"

# --- Cadence binaries ------------------------------------------------------
# name -> path in the image. The guest finds them under /proc/boot.
CADENCE_CANDIDATES=(
    "cadenced:src/daemon/cadenced"
    "cadence-cli:src/client/cadence-cli"
    "ctrl_task:src/rt/ctrl_task"
)
find_binary() {
    local rel="$1" dir
    for dir in ${CADENCE_BIN_DIR:-} "${SEARCH_DIRS[@]:-}" "$REPO_ROOT"; do
        [ -n "$dir" ] || continue
        if [ -x "$dir/$rel" ]; then
            printf '%s\n' "$dir/$rel"
            return 0
        fi
    done
    return 1
}

CADENCE_BLOCK=""
for entry in "${CADENCE_CANDIDATES[@]}"; do
    name="${entry%%:*}"
    rel="${entry#*:}"
    if path="$(find_binary "$rel")"; then
        log "cadence binary: $name <- $path"
        CADENCE_BLOCK+="${name} = ${path}"$'\n'
    else
        log "cadence binary NOT found: $rel (left as a placeholder)"
        CADENCE_BLOCK+="# ${name} = <path>/${rel}   # not built yet — see images/buildfile.gen.sh"$'\n'
    fi
done

# --- model -----------------------------------------------------------------
# --embed-model puts the GGUF inside the IFS (self-contained image, ~570 MB
# bigger). Without it, the model is carried by the separate qnx6 disk image
# and mounted on /model at boot time.
MODEL_LINE=""
if [ -n "$MODEL_FILE" ] && [ "$EMBED_MODEL" -eq 1 ]; then
    [ -f "$MODEL_FILE" ] || die "model file not found: $MODEL_FILE"
    log "embedding  : $MODEL_FILE"
    MODEL_LINE="model.gguf = ${MODEL_FILE}"
else
    log "model      : ${MODEL_FILE:-none} (carried by the separate qnx6 disk image)"
fi

# --- substitution ----------------------------------------------------------
# The Cadence block may span several lines, so it is written to a file and
# injected with sed's "r" command rather than inlined in the s/// expression.
CADENCE_FILE="$(mktemp /tmp/cadence-binaries.XXXXXX)"
trap 'rm -f "$CADENCE_FILE"' EXIT
printf '%s' "$CADENCE_BLOCK" > "$CADENCE_FILE"

MODEL_FILE_TMP="$(mktemp /tmp/cadence-model-line.XXXXXX)"
printf '%s' "$MODEL_LINE" > "$MODEL_FILE_TMP"

sed_script="$(mktemp /tmp/buildfile.gen.XXXXXX.sed)"
{
    printf 's|@QNX_TARGET@|%s|g\n' "$QNX_ARCH_TARGET"
    printf '/^@CADENCE_BINARIES@$/r %s\n' "$CADENCE_FILE"
    printf '/^@CADENCE_BINARIES@$/d\n'
    printf '/^@MODEL@$/r %s\n' "$MODEL_FILE_TMP"
    printf '/^@MODEL@$/d\n'
} > "$sed_script"

sed -f "$sed_script" "$TEMPLATE" > "$OUT"
rm -f "$sed_script" "$MODEL_FILE_TMP"

# The model is not part of the IFS: it lives on its own qnx6 disk image.
# Reference it in a comment so the build is self-documenting.
if [ -n "$MODEL_FILE" ]; then
    log "model      : $MODEL_FILE (mounted at /model in the guest, not in the IFS)"
fi

log "written    : $OUT"

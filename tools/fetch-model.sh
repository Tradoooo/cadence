#!/usr/bin/env bash
# fetch-model.sh — download the Ternary-Bonsai 2B Q2_0 GGUF model.
#
# Uses curl or wget (no `hf` / huggingface-cli dependency) and verifies the
# SHA-256 checksum before the file is considered usable.
#
# Usage:
#   tools/fetch-model.sh [--out PATH]
#
# Environment:
#   MODEL_URL   override the download URL
#   MODEL_SHA   override the expected SHA-256
#   MODEL_SIZE  override the expected size in bytes

set -euo pipefail

MODEL_FILE="Ternary-Bonsai-2-2B-Q2_0.gguf"
MODEL_REPO="prism-ml/Ternary-Bonsai-2-2B-gguf"
MODEL_URL="${MODEL_URL:-https://huggingface.co/${MODEL_REPO}/resolve/main/${MODEL_FILE}?download=true}"
MODEL_SHA="${MODEL_SHA:-c2893fa6f951daee8b7263d5f49b455e57989ef56417a22125b03566dc560650}"
MODEL_SIZE="${MODEL_SIZE:-546689504}"

OUT="${MODEL_OUT:-}"

log() { printf '[fetch-model] %s\n' "$*"; }
die() { printf '[fetch-model] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -n "$OUT" ] || OUT="$(pwd)/$MODEL_FILE"

OUT_DIR="$(dirname "$OUT")"
mkdir -p "$OUT_DIR"

# --- checksum helper -------------------------------------------------------
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    fi
}

size_of() {
    if command -v stat >/dev/null 2>&1; then
        stat -c %s "$1"
    else
        wc -c < "$1" | tr -d ' '
    fi
}

# --- already downloaded and valid? ----------------------------------------
if [ -f "$OUT" ]; then
    have_sha="$(sha256_of "$OUT")"
    if [ "$have_sha" = "$MODEL_SHA" ]; then
        log "already present and valid: $OUT"
        log "sha256 = $have_sha"
        exit 0
    fi
    log "present but checksum mismatch (got $have_sha), re-downloading"
    rm -f "$OUT"
fi

# --- download --------------------------------------------------------------
if command -v curl >/dev/null 2>&1; then
    log "downloading with curl"
    log "  url    : $MODEL_URL"
    curl -fL --retry 3 --retry-delay 5 --connect-timeout 30 \
         -o "$OUT.part" "$MODEL_URL" \
        || die "download failed (curl)"
elif command -v wget >/dev/null 2>&1; then
    log "downloading with wget"
    log "  url    : $MODEL_URL"
    wget --tries=3 --timeout=30 -O "$OUT.part" "$MODEL_URL" \
        || die "download failed (wget)"
else
    die "neither curl nor wget is available"
fi

mv -f "$OUT.part" "$OUT"

# --- verify ----------------------------------------------------------------
have_size="$(size_of "$OUT")"
[ "$have_size" = "$MODEL_SIZE" ] \
    || die "size mismatch: expected $MODEL_SIZE bytes, got $have_size"

have_sha="$(sha256_of "$OUT")"
[ "$have_sha" = "$MODEL_SHA" ] \
    || die "sha256 mismatch: expected $MODEL_SHA, got $have_sha"

log "OK — $OUT"
log "  size   : $have_size bytes"
log "  sha256 : $have_sha"

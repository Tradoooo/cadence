# Cadence — QNX Neutrino porting layer
#
# Top-level entry points. Everything is driven from the repository root:
#
#   make help        list the available targets
#   make check       verify the patch applies on a pristine upstream checkout
#   make llama       build llama.cpp for QNX (needs the QNX SDP 8.0)
#   make model       download + checksum the GGUF model
#   make ifs         build the IFS boot image and the model disk
#   make run         boot the image under QEMU (KVM) and capture the console
#   make bench       boot and run the in-guest benchmark
#
# The build steps need a machine with the QNX SDP installed (QNX_HOST and
# QNX_TARGET). They fail with an explicit message elsewhere.

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.DEFAULT_GOAL := help

# --- layout -----------------------------------------------------------------
BUILD_DIR   := build
LLAMA_DIR   := $(BUILD_DIR)/llama
IMAGE_DIR   := $(BUILD_DIR)/image
WORK_DIR    := $(BUILD_DIR)/work

# --- upstream pin -----------------------------------------------------------
LLAMA_REPO    ?= https://github.com/ggml-org/llama.cpp
LLAMA_COMMIT  ?= 9daa0aa22b8aa0143344d7549dee1f0e32f048bb

# --- model ------------------------------------------------------------------
MODEL_FILE   ?= $(BUILD_DIR)/models/Ternary-Bonsai-2-2B-Q2_0.gguf
MODEL_URL    ?= https://huggingface.co/prism-ml/Ternary-Bonsai-2-2B-gguf/resolve/main/Ternary-Bonsai-2-2B-Q2_0.gguf?download=true
MODEL_SHA    ?= c2893fa6f951daee8b7263d5f49b455e57989ef56417a22125b03566dc560650
MODEL_SIZE   ?= 546689504

# --- SDP discovery ----------------------------------------------------------
QNX_SDP_ROOT ?= $(firstword $(wildcard $(HOME)/qnx800 /opt/qnx800 /opt/qnx/qnxsdp-8.0))
QNX_ENV      := $(QNX_SDP_ROOT)/qnxsdp-env.sh

# --- tools ------------------------------------------------------------------
QEMU         ?= qemu-system-x86_64
ACCEL        ?= kvm
JOBS         ?= $(shell nproc 2>/dev/null || echo 4)

export QNX_SDP_ROOT
export LLAMA_CPP_URL       := $(LLAMA_REPO)
export LLAMA_CPP_COMMIT    := $(LLAMA_COMMIT)
export MODEL_URL
export MODEL_SHA
export MODEL_SIZE

# --- helpers ----------------------------------------------------------------
# Fail early with a readable message when the QNX SDP is missing.
define require_sdp
$(if $(wildcard $(QNX_ENV)),,$(error QNX SDP 8.0 not found. Install it under ~/qnx800 (or set QNX_SDP_ROOT) and re-run. The QNX targets need qcc/q++ from the SDP host tools and the x86_64 target libraries.))
endef

.PHONY: help
help: ## show this help
	@printf 'Cadence — QNX porting layer\n\n'
	@printf 'Targets:\n'
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	  | awk 'BEGIN {FS = ":.*?## "} {printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}'
	@printf '\nVariables:\n'
	@printf '  QNX_SDP_ROOT   QNX SDP 8.0 location (default: %s)\n' '$$QNX_SDP_ROOT'
	@printf '  ACCEL          kvm or tcg (default: %s)\n' '$(ACCEL)'
	@printf '  MODEL_FILE     where the GGUF model is stored\n'

# --- the port ---------------------------------------------------------------
.PHONY: check
check: ## verify the patch applies on a pristine upstream checkout
	@require_sdp
	@echo "== checking patches/0001-qnx-port.patch against $(LLAMA_COMMIT) =="
	@rm -rf $(WORK_DIR)/check
	@mkdir -p $(WORK_DIR)/check
	@cd $(WORK_DIR)/check \
	  && git init -q \
	  && git remote add origin $(LLAMA_REPO) \
	  && (git fetch -q --depth 1 origin $(LLAMA_COMMIT) || git fetch -q --filter=blob:none origin) \
	  && git checkout -q $(LLAMA_COMMIT)
	@cd $(WORK_DIR)/check && git apply --check $(CURDIR)/patches/0001-qnx-port.patch
	@echo "OK: the patch applies cleanly on $(LLAMA_COMMIT)"

.PHONY: llama
llama: ## build llama.cpp for QNX (patched)
	@require_sdp
	@mkdir -p $(BUILD_DIR)
	@tools/build-llama.sh --workdir $(LLAMA_DIR) --jobs $(JOBS)

# --- the model --------------------------------------------------------------
.PHONY: model
model: ## download the GGUF model and verify its SHA-256
	@mkdir -p $(dir $(MODEL_FILE))
	@tools/fetch-model.sh --out $(MODEL_FILE)

# --- the image --------------------------------------------------------------
.PHONY: ifs
ifs: ## build the IFS boot image and the model disk
	@require_sdp
	@mkdir -p $(IMAGE_DIR)
	@images/build-image.sh \
	    --outdir $(IMAGE_DIR) \
	    --bin-dir $(LLAMA_DIR)/sysroot/bin \
	    --model $(MODEL_FILE)

.PHONY: ifs-only
ifs-only: ## build the IFS boot image only (model stays on its own disk)
	@require_sdp
	@mkdir -p $(IMAGE_DIR)
	@images/build-image.sh \
	    --outdir $(IMAGE_DIR) \
	    --bin-dir $(LLAMA_DIR)/sysroot/bin \
	    --model $(MODEL_FILE) \
	    --skip-model-disk

.PHONY: model-disk
model-disk: ## build only the qnx6 disk that carries the model
	@require_sdp
	@mkdir -p $(IMAGE_DIR)
	@images/make-model-disk.sh --model $(MODEL_FILE) --out $(IMAGE_DIR)/cadence-model.part

# --- running ----------------------------------------------------------------
.PHONY: run
run: ## boot the image under QEMU (ACCEL=kvm|tcg) and capture the console
	@images/run-qemu.sh --accel $(ACCEL) \
	    --ifs $(IMAGE_DIR)/cadence-ifs.bin \
	    --model-disk $(IMAGE_DIR)/cadence-model.part

.PHONY: bench
bench: ## boot the image and run the in-guest benchmark
	@images/run-qemu.sh --accel $(ACCEL) --timeout 900 \
	    --ifs $(IMAGE_DIR)/cadence-ifs.bin \
	    --model-disk $(IMAGE_DIR)/cadence-model.part
	@echo "--- benchmark results ---"
	@grep -E "t/s|model  *\||pp[0-9]|tg[0-9]" $(IMAGE_DIR)/serial-$(ACCEL).log || true

# --- housekeeping -----------------------------------------------------------
.PHONY: clean
clean: ## remove the build outputs (keeps the downloaded model)
	@rm -rf $(BUILD_DIR)/llama $(BUILD_DIR)/image $(BUILD_DIR)/work
	@echo "cleaned"

.PHONY: distclean
distclean: ## remove every build output, including the model
	@rm -rf $(BUILD_DIR)
	@echo "cleaned (model included)"

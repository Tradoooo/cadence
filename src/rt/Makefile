# Makefile -- Cadence real-time proof tools (QNX 8.0, x86_64).
#
#   make            build ctrl_task, latency_probe, trace_dump
#   make ctrl_task  just the control task
#
# The QNX SDP 8.0 must be sourced first:
#   . ~/qnx800/qnxsdp-env.sh
#
# latency_probe links against the llama.cpp QNX port (static libraries).
# Override LLAMA_ROOT / LLAMA_BUILD if the port lives elsewhere.

QCC        ?= qcc
CFLAGS     ?= -O2 -Wall -Wextra -std=gnu11
LLAMA_ROOT ?= $(HOME)/qnx-llama/llama.cpp
LLAMA_BUILD ?= $(LLAMA_ROOT)/build-qnx

LLAMA_INC  = -I$(LLAMA_ROOT)/include -I$(LLAMA_ROOT)/ggml/include
LLAMA_LIBS = $(LLAMA_BUILD)/src/libllama.a \
              $(LLAMA_BUILD)/ggml/src/libggml.a \
              $(LLAMA_BUILD)/ggml/src/libggml-cpu.a \
              $(LLAMA_BUILD)/ggml/src/libggml-base.a \
              $(LLAMA_BUILD)/ggml/src/libggml-quants.a

BINS = ctrl_task latency_probe trace_dump rt_sleep

all: $(BINS)

ctrl_task: ctrl_task.c ctrl_report.c ctrl_types.h rt_common.h
	$(QCC) $(CFLAGS) -o $@ ctrl_task.c ctrl_report.c -lm

trace_dump: trace_dump.c
	$(QCC) $(CFLAGS) -o $@ trace_dump.c

rt_sleep: rt_sleep.c
	$(QCC) $(CFLAGS) -o $@ rt_sleep.c

latency_probe: latency_probe.c rt_common.h
	$(QCC) $(CFLAGS) $(LLAMA_INC) -o $@ latency_probe.c $(LLAMA_LIBS) -lpthread -lm

clean:
	rm -f $(BINS)

.PHONY: all clean

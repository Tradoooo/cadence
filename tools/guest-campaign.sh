#!/bin/sh
# guest-campaign.sh -- runs INSIDE the QNX 8.0 guest (ksh).
#
# Campaign executed for the real-time proof:
#   phase 1  control task alone (idle baseline)
#   phase 2  control task (isolated: SCHED_FIFO 63 + CPU 0) WHILE the local
#            LLM generates a long answer, with tracelogger capturing the
#            kernel trace
#   phase 3  control task NOT isolated (default priority 10, no pinning)
#            WHILE the LLM generates -> shows what the isolation buys
#
# Every JSON artefact is written to /dev/shmem (RAM filesystem) and then
# echoed to the serial console between markers so the host can pick them up.
#
# Deliberately avoids toybox applets (they do not dispatch in this image):
# only the binaries of this project, pidin and kill are used.

PATH=/proc/boot:/system/bin
export PATH
HOME=/proc/boot
export HOME
TERM=qansi
export TERM

SH=/dev/shmem
CT=/proc/boot/ctrl_task
LP=/proc/boot/latency_probe
TD=/proc/boot/trace_dump
MODEL=/proc/boot/model.gguf

mkdir -p $SH 2>/dev/null

echo "##### CADENCE RT CAMPAIGN START #####"
pidin info
pidin arg

echo "##### PHASE 1 : control task alone (idle baseline) #####"
$CT -p 1000 -d 15 -P 63 -c 0 -t idle -o $SH/ctrl_idle.json > /dev/null
echo "<<<JSON_BEGIN ctrl_idle>>>"
$TD $SH/ctrl_idle.json -
echo "<<<JSON_END ctrl_idle>>>"

echo "##### PHASE 2 : LLM + isolated control task + tracelogger #####"
/proc/boot/tracelogger-run.sh -d 45 -f $SH/trace.kev &
TLPID=$!
/proc/boot/rt_sleep 3
$CT -p 1000 -d 38 -P 63 -c 0 -t llm_isolated -o $SH/ctrl_llm_isolated.json > /dev/null &
CTPID=$!
/proc/boot/rt_sleep 1
$LP -m $MODEL -n 300 -t 5 --ignore-eog -o $SH/probe_llm.json > /dev/null 2> $SH/probe_llm.txt
echo "<<<TEXT_BEGIN probe_llm>>>"
$TD $SH/probe_llm.txt -
echo "<<<TEXT_END probe_llm>>>"
echo "<<<JSON_BEGIN probe_llm>>>"
$TD $SH/probe_llm.json -
echo "<<<JSON_END probe_llm>>>"
wait $CTPID
echo "<<<JSON_BEGIN ctrl_llm_isolated>>>"
$TD $SH/ctrl_llm_isolated.json -
echo "<<<JSON_END ctrl_llm_isolated>>>"
wait $TLPID

echo "##### PHASE 3 : LLM + non-isolated control task (prio 10, no pinning) #####"
$CT -p 1000 -d 18 -P 10 -c -1 -t llm_shared -o $SH/ctrl_llm_shared.json > /dev/null &
CTPID=$!
/proc/boot/rt_sleep 1
$LP -m $MODEL -n 200 -t 6 --ignore-eog -o $SH/probe_llm_shared.json > /dev/null 2> $SH/probe_llm_shared.txt
echo "<<<TEXT_BEGIN probe_llm_shared>>>"
$TD $SH/probe_llm_shared.txt -
echo "<<<TEXT_END probe_llm_shared>>>"
echo "<<<JSON_BEGIN probe_llm_shared>>>"
$TD $SH/probe_llm_shared.json -
echo "<<<JSON_END probe_llm_shared>>>"
wait $CTPID
echo "<<<JSON_BEGIN ctrl_llm_shared>>>"
$TD $SH/ctrl_llm_shared.json -
echo "<<<JSON_END ctrl_llm_shared>>>"

echo "##### EXTRACT TRACE (base64 on the console) #####"
if [ -f $SH/trace.kev ]; then
    echo "<<<TRACE_BEGIN>>>"
    $TD --base64 $SH/trace.kev
    echo "<<<TRACE_END>>>"
else
    echo "no trace file"
fi

echo "##### CADENCE RT CAMPAIGN DONE #####"

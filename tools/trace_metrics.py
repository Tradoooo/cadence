#!/usr/bin/env python3
"""trace_metrics.py -- HOST side metrics for a QNX kernel trace.

Parses the readable output of traceprinter (one event per line) and
produces a JSON summary: event counts per class/event, interrupt rate,
preemption count and the worst gaps.

Usage: trace_metrics.py <trace.txt> <out.json>
"""
import json
import re
import sys
from collections import Counter

LINE_RE = re.compile(r"t:(?P<ts>[0-9.]+)(?:us)?\s+CPU:(?P<cpu>\d+)\s+(?P<cls>[A-Z_ ]+):\s*(?P<ev>\S+)")
TS_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)*$")


def parse_ts(text):
    """Convert a traceprinter timestamp into microseconds.

    traceprinter prints the timestamp as a dotted decimal
    (seconds.microseconds.nanoseconds for long traces, plain microseconds
    for short ones).  Both forms are accepted.
    """
    parts = text.split(".")
    try:
        nums = [float(p) for p in parts]
    except ValueError:
        return None
    if len(nums) >= 3:
        return nums[0] * 1e6 + nums[1] + nums[2] / 1000.0
    if len(nums) == 2:
        return nums[0] * 1e6 + nums[1]
    return nums[0]


def read_events(path):
    events = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = LINE_RE.search(line)
            if not m:
                continue
            ts = parse_ts(m.group("ts"))
            if ts is None:
                continue
            events.append({
                "ts_us": ts,
                "cpu": int(m.group("cpu")),
                "class": m.group("cls").strip(),
                "event": m.group("ev").strip(),
            })
    return events


def percentile(values, pct):
    if not values:
        return None
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round(pct / 100.0 * len(s) + 0.5)) - 1))
    return s[k]


def gap_stats(events, key):
    """Statistics on the gaps (us) between consecutive events of a kind."""
    times = sorted(e["ts_us"] for e in events if e["event"] == key)
    gaps = [b - a for a, b in zip(times, times[1:])]
    if not gaps:
        return {"count": len(times), "gap_us": {}}
    return {
        "count": len(times),
        "gap_us": {
            "min": round(min(gaps), 3),
            "mean": round(sum(gaps) / len(gaps), 3),
            "max": round(max(gaps), 3),
            "p99": round(percentile(gaps, 99) or 0.0, 3),
        },
        "span_us": round(times[-1] - times[0], 3),
    }


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    events = read_events(src)
    if not events:
        with open(dst, "w") as fh:
            json.dump({"events": 0, "note": "no event matched the parser"}, fh, indent=2)
        print("trace_metrics: no event parsed from", src, file=sys.stderr)
        return 1

    by_event = Counter("%s:%s" % (e["class"], e["event"]) for e in events)
    span = max(e["ts_us"] for e in events) - min(e["ts_us"] for e in events)
    duration_s = span / 1e6 if span > 0 else 0.0

    interrupts = [e for e in events if "interrupt" in e["class"].lower()]
    preempted = [e for e in events
                 if "preempt" in e["event"].lower() or e["event"] == "THINTR"]
    switches = [e for e in events if e["event"] == "THRUNNING"]
    ipis = [e for e in events if "IPI" in e["event"]]

    metrics = {
        "events": len(events),
        "span_us": round(span, 3),
        "duration_s": round(duration_s, 6),
        "by_event": dict(by_event.most_common()),
        "interrupt_count": len(interrupts),
        "interrupt_rate_hz": round(len(interrupts) / duration_s, 2) if duration_s > 0 else 0.0,
        "preemption_count": len(preempted),
        "preemption_rate_hz": round(len(preempted) / duration_s, 2) if duration_s > 0 else 0.0,
        "thread_switch_count": len(switches),
        "thread_switch_rate_hz": round(len(switches) / duration_s, 2) if duration_s > 0 else 0.0,
        "ipi_count": len(ipis),
        "ipi_rate_hz": round(len(ipis) / duration_s, 2) if duration_s > 0 else 0.0,
        "interrupts": gap_stats(events, "InterruptKernel"),
        "per_cpu": dict(Counter(str(e["cpu"]) for e in events)),
    }
    with open(dst, "w") as fh:
        json.dump(metrics, fh, indent=2)
    print("trace_metrics: %d events, %d interrupts, %d preemptions -> %s"
          % (metrics["events"], metrics["interrupt_count"],
             metrics["preemption_count"], dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())

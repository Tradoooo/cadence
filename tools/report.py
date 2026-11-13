#!/usr/bin/env python3
"""report.py -- HOST side report generator for the Cadence real-time proof.

Aggregates:
  * the control task statistics (jitter min/mean/max/p99, deadline misses)
    for every phase (idle / LLM isolated / LLM shared),
  * the per-token latency statistics and throughput of the LLM probe,
  * the kernel trace metrics (interrupt count, preemptions),
and produces report.json + REPORT.md.

The JSON artefacts are extracted from the QNX serial log (the guest prints
them between <<<JSON_BEGIN name>>> / <<<JSON_END name>>> markers).

Usage:
  report.py --serial <serial.log> --out <outdir> [--trace-metrics <json>]
"""
import argparse
import json
import os
import re
import sys

BEGIN_RE = re.compile(r"<<<JSON_BEGIN\s+([A-Za-z0-9_.-]+)>>>")
END_RE = re.compile(r"<<<JSON_END\s+([A-Za-z0-9_.-]+)>>>")


def extract_blocks(serial_path):
    """Return {name: parsed_json} for every marked block of the serial log."""
    blocks = {}
    current = None
    payload = []
    with open(serial_path, "r", errors="replace") as fh:
        for line in fh:
            m = BEGIN_RE.search(line)
            if m:
                current = m.group(1)
                payload = []
                continue
            m = END_RE.search(line)
            if m and current:
                blocks[current] = "\n".join(payload)
                current = None
                payload = []
                continue
            if current is not None and line.strip().startswith("{"):
                payload.append(line.strip())
    out = {}
    for name, text in blocks.items():
        try:
            out[name] = json.loads(text)
        except ValueError:
            # the block may contain several lines: take the last JSON object
            for line in reversed(text.splitlines()):
                try:
                    out[name] = json.loads(line)
                    break
                except ValueError:
                    continue
            else:
                print("report: cannot parse JSON block %s" % name, file=sys.stderr)
    return out


def ctrl_row(doc):
    """Flatten a control-task JSON into a table row."""
    if not doc:
        return None
    j = doc.get("jitter_us", {})
    return {
        "phase": doc.get("tag"),
        "priority": doc.get("priority"),
        "cpu": doc.get("cpu"),
        "affinity": doc.get("affinity_applied"),
        "period_us": round(doc.get("period_ns", 0) / 1000.0, 1),
        "cycles": doc.get("cycles"),
        "deadline_misses": doc.get("deadline_misses"),
        "skipped_periods": doc.get("skipped_periods"),
        "jitter_min_us": j.get("min"),
        "jitter_mean_us": j.get("mean"),
        "jitter_max_us": j.get("max"),
        "jitter_p99_us": j.get("p99"),
        "work_max_us": doc.get("work_us", {}).get("max"),
    }


def probe_row(doc):
    if not doc:
        return None
    t = doc.get("token_latency_ms", {})
    return {
        "tag": doc.get("tag"),
        "tokens": doc.get("tokens"),
        "threads": doc.get("n_threads"),
        "generation_s": doc.get("generation_s"),
        "throughput_tps": doc.get("throughput_tps"),
        "lat_min_ms": t.get("min"),
        "lat_mean_ms": t.get("mean"),
        "lat_max_ms": t.get("max"),
        "lat_p99_ms": t.get("p99"),
        "model_load_s": doc.get("model_load_s"),
    }


def fmt(v, digits=2, dash="-"):
    if v is None:
        return dash
    if isinstance(v, (int,)) and not isinstance(v, bool):
        return str(v)
    try:
        return ("%." + str(digits) + "f") % float(v)
    except (TypeError, ValueError):
        return str(v)


def table(rows, headers):
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join(["---"] * len(headers)) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


def build_markdown(rep):
    proof = rep["proof"]
    lines = []
    lines.append("PREUVE RT : %d cycles, %d dépassements, gigue max %s us, "
                 "latence token p99 %s ms"
                 % (proof["cycles"], proof["deadline_misses"],
                    fmt(proof["jitter_max_us"], 1), fmt(proof["token_p99_ms"], 2)))
    lines.append("")
    lines.append("# Cadence -- preuve temps reel (QNX 8.0, invite KVM)")
    lines.append("")
    lines.append("## 1. Verdict")
    lines.append("")
    lines.append("| critere | valeur |")
    lines.append("|---|---|")
    lines.append("| cycles de controle (phase LLM isole) | %d |" % proof["cycles"])
    lines.append("| depassements de periode | %d |" % proof["deadline_misses"])
    lines.append("| gigue max | %s us |" % fmt(proof["jitter_max_us"], 1))
    lines.append("| gigue p99 | %s us |" % fmt(proof["jitter_p99_us"], 1))
    lines.append("| latence token p99 | %s ms |" % fmt(proof["token_p99_ms"], 2))
    lines.append("| debit LLM | %s t/s |" % fmt(proof["throughput_tps"], 2))
    lines.append("| tokens generes | %s |" % fmt(proof["tokens"], 0))
    lines.append("")
    lines.append("## 2. Tache de controle -- jitter et depassements")
    lines.append("")
    lines.append(rep["ctrl_table"])
    lines.append("")
    lines.append("## 3. LLM -- latence par token")
    lines.append("")
    lines.append(rep["probe_table"])
    lines.append("")
    if rep.get("trace_table"):
        lines.append("## 4. Trace kernel (tracelogger / traceprinter)")
        lines.append("")
        lines.append(rep["trace_table"])
        lines.append("")
    lines.append("## 5. Fichiers")
    lines.append("")
    for f in rep["files"]:
        lines.append("- `%s`" % f)
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--trace-metrics", default=None)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    blocks = extract_blocks(args.serial)

    ctrl_rows = []
    for name in sorted(blocks):
        if name.startswith("ctrl_"):
            row = ctrl_row(blocks[name])
            if row:
                ctrl_rows.append(row)
    probe_rows = []
    for name in sorted(blocks):
        if name.startswith("probe_"):
            row = probe_row(blocks[name])
            if row:
                probe_rows.append(row)

    proof_doc = blocks.get("ctrl_llm_isolated") or {}
    probe_doc = blocks.get("probe_llm") or {}
    j = proof_doc.get("jitter_us", {})
    t = probe_doc.get("token_latency_ms", {})
    proof = {
        "cycles": proof_doc.get("cycles", 0),
        "deadline_misses": proof_doc.get("deadline_misses", 0),
        "jitter_max_us": j.get("max"),
        "jitter_p99_us": j.get("p99"),
        "token_p99_ms": t.get("p99"),
        "throughput_tps": probe_doc.get("throughput_tps"),
        "tokens": probe_doc.get("tokens"),
    }

    ctrl_table = table(
        [[r["phase"], r["priority"], r["cpu"], r["period_us"], r["cycles"],
          r["deadline_misses"], fmt(r["jitter_min_us"], 1), fmt(r["jitter_mean_us"], 1),
          fmt(r["jitter_p99_us"], 1), fmt(r["jitter_max_us"], 1),
          fmt(r["work_max_us"], 1)] for r in ctrl_rows],
        ["phase", "prio", "cpu", "periode(us)", "cycles", "depassements",
         "jitter min(us)", "jitter moy(us)", "jitter p99(us)", "jitter max(us)",
         "travail max(us)"])

    probe_table = table(
        [[r["tag"], r["threads"], r["tokens"], fmt(r["model_load_s"], 2),
          fmt(r["generation_s"], 2), fmt(r["throughput_tps"], 2),
          fmt(r["lat_min_ms"], 2), fmt(r["lat_mean_ms"], 2),
          fmt(r["lat_p99_ms"], 2), fmt(r["lat_max_ms"], 2)] for r in probe_rows],
        ["phase", "threads", "tokens", "chargement(s)", "generation(s)",
         "debit(t/s)", "lat min(ms)", "lat moy(ms)", "lat p99(ms)", "lat max(ms)"])

    trace_table = ""
    trace_metrics = None
    if args.trace_metrics and os.path.exists(args.trace_metrics):
        with open(args.trace_metrics) as fh:
            trace_metrics = json.load(fh)
        ev = trace_metrics.get("by_event", {})
        rows = [[k, v] for k, v in list(ev.items())[:15]]
        trace_table = table(rows, ["evenement (classe:evenement)", "nombre"])
        trace_table += "\n\n"
        trace_table += " interruptions: %s | preemptions: %s | duree trace: %s s" % (
            trace_metrics.get("interrupt_count"),
            trace_metrics.get("preemption_count"),
            fmt(trace_metrics.get("duration_s"), 3))

    files = sorted(os.path.join(args.out, f) for f in os.listdir(args.out))
    files = [os.path.basename(f) for f in files] + ["docs/REAL-TIME-PROOF.md"]

    rep = {
        "proof": proof,
        "control_task": ctrl_rows,
        "llm_probe": probe_rows,
        "trace_metrics": trace_metrics,
        "ctrl_table": ctrl_table,
        "probe_table": probe_table,
        "trace_table": trace_table,
        "files": files,
    }
    with open(os.path.join(args.out, "report.json"), "w") as fh:
        json.dump(rep, fh, indent=2)
    with open(os.path.join(args.out, "REPORT.md"), "w") as fh:
        fh.write(build_markdown(rep))
    print("report: wrote %s/report.json and %s/REPORT.md" % (args.out, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())

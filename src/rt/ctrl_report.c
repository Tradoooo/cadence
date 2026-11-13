/*
 * ctrl_report.c -- JSON report of the Cadence control task (QNX 8.0).
 *
 * Exposes the whole measurement as a single JSON object: configuration,
 * jitter statistics (min / mean / max / p50 / p95 / p99 / stddev), deadline
 * misses, work time and the 20 worst events with their timestamp.
 */
#include <stdio.h>

#include "ctrl_types.h"
#include "rt_common.h"

static void emit_stats(rt_json *j, const char *key, const rt_stats *s, double scale)
{
    rt_json_obj_open(j, key);
    rt_json_u64(j, "count", s->count);
    rt_json_num(j, "min", (double)s->min / scale, 3);
    rt_json_num(j, "mean", s->mean / scale, 3);
    rt_json_num(j, "max", (double)s->max / scale, 3);
    rt_json_num(j, "p50", (double)s->p50 / scale, 3);
    rt_json_num(j, "p95", (double)s->p95 / scale, 3);
    rt_json_num(j, "p99", (double)s->p99 / scale, 3);
    rt_json_num(j, "stddev", s->stddev / scale, 3);
    rt_json_obj_close(j);
}

static void emit_worst(rt_json *j, const struct rt_state *st)
{
    size_t i;

    rt_json_arr_open(j, "worst");
    for (i = 0; i < st->nworst; i++) {
        rt_json_obj_open(j, NULL);
        rt_json_u64(j, "cycle", st->worst[i].cycle);
        rt_json_u64(j, "t_ns", st->worst[i].t_ns);
        rt_json_num(j, "jitter_us", (double)st->worst[i].jitter_ns / 1000.0, 3);
        rt_json_u64(j, "skipped_periods", st->worst[i].skipped);
        rt_json_obj_close(j);
    }
    rt_json_arr_close(j);
}

void ctrl_emit_report(struct rt_state *st, const rt_stats *jst, rt_json *j)
{
    uint64_t end_ns = rt_now_ns();

    rt_json_obj_open(j, NULL);
    rt_json_str(j, "tool", "ctrl_task");
    rt_json_str(j, "tag", st->cfg.tag);
    rt_json_str(j, "host", "qnx8.0-x86_64");
    rt_json_u64(j, "period_ns", st->cfg.period_ns);
    rt_json_num(j, "target_hz", 1e9 / (double)st->cfg.period_ns, 3);
    rt_json_str(j, "sched", "SCHED_FIFO");
    rt_json_u64(j, "priority", (uint64_t)st->cfg.priority);
    rt_json_bool(j, "priority_applied", st->prio_ok);
    rt_json_u64(j, "kernel_tick_us", RT_KERNEL_TICK_NS / 1000u);
    rt_json_bool(j, "kernel_tick_applied", st->cp_changed);
    rt_json_u64(j, "kernel_tick_errno", (uint64_t)st->cp_errno);
    rt_json_bool(j, "hi_res_timer", st->hi_res);
    rt_json_num(j, "cpu", (double)st->cfg.cpu, 0);
    rt_json_bool(j, "affinity_applied", st->affinity_ok);
    rt_json_u64(j, "affinity_errno", (uint64_t)st->affinity_errno);
    rt_json_u64(j, "start_ns", st->start_ns);
    rt_json_u64(j, "end_ns", end_ns);
    rt_json_num(j, "duration_s", (double)(end_ns - st->start_ns) / 1e9, 3);
    rt_json_u64(j, "cycles", st->cycles);
    rt_json_u64(j, "deadline_misses", st->deadline_misses);
    rt_json_u64(j, "miss_events", st->miss_events);
    rt_json_u64(j, "skipped_periods", st->skipped_periods);
    rt_json_bool(j, "clean", st->deadline_misses == 0);
    emit_stats(j, "jitter_us", jst, 1000.0);
    emit_stats(j, "jitter_ns", jst, 1.0);
    rt_json_obj_open(j, "work_us");
    rt_json_num(j, "min", (double)st->work_min_ns / 1000.0, 3);
    rt_json_num(j, "mean",
                (double)st->work_sum_ns / (double)(st->cycles ? st->cycles : 1) / 1000.0, 3);
    rt_json_num(j, "max", (double)st->work_max_ns / 1000.0, 3);
    rt_json_obj_close(j);
    emit_worst(j, st);
    rt_json_obj_close(j);
}

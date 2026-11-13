/*
 * rt_common.h -- shared helpers for the Cadence real-time proof (QNX 8.0).
 *
 * Used by src/rt/ctrl_task.c (jitter of the control task) and
 * src/rt/latency_probe.c (per-token latency of the LLM).  Keeping the
 * clock, JSON and statistics code here avoids duplicating it.
 *
 * QNX Neutrino / C11, no dynamic allocation hidden behind the API.
 */
#ifndef RT_COMMON_H
#define RT_COMMON_H

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ time */

static inline uint64_t rt_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t rt_ts_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static inline void rt_ns_to_ts(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

/* ------------------------------------------------------------------ JSON */

#define RT_JSON_MAX_DEPTH 8

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    int     depth;
    int     sep[RT_JSON_MAX_DEPTH];
} rt_json;

static inline int rt_json_init(rt_json *j, size_t cap)
{
    j->buf = malloc(cap);
    if (j->buf == NULL) {
        return -1;
    }
    j->buf[0] = '\0';
    j->len     = 0;
    j->cap     = cap;
    j->depth   = 0;
    j->sep[0]  = 0;
    return 0;
}

static inline void rt_json_free(rt_json *j)
{
    free(j->buf);
    j->buf = NULL;
}

static inline int rt_json_reserve(rt_json *j, size_t need)
{
    size_t want;
    char  *nb;

    if (j->len + need + 1 <= j->cap) {
        return 0;
    }
    want = j->cap ? j->cap : 256;
    while (want < j->len + need + 1) {
        want *= 2;
    }
    nb = realloc(j->buf, want);
    if (nb == NULL) {
        return -1;
    }
    j->buf = nb;
    j->cap = want;
    return 0;
}

static inline void rt_json_raw(rt_json *j, const char *s)
{
    size_t n = strlen(s);

    if (rt_json_reserve(j, n) != 0) {
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

static inline void rt_json_printf(rt_json *j, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || rt_json_reserve(j, (size_t)n) != 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(j->buf + j->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    j->len += (size_t)n;
}

static inline void rt_json_sep(rt_json *j)
{
    if (j->sep[j->depth]) {
        rt_json_raw(j, ",");
    }
    j->sep[j->depth] = 1;
}

static inline void rt_json_obj_open(rt_json *j, const char *key)
{
    rt_json_sep(j);
    if (key != NULL) {
        rt_json_printf(j, "\"%s\":{", key);
    } else {
        rt_json_raw(j, "{");
    }
    if (j->depth + 1 < RT_JSON_MAX_DEPTH) {
        j->depth++;
        j->sep[j->depth] = 0;
    }
}

static inline void rt_json_obj_close(rt_json *j)
{
    rt_json_raw(j, "}");
    if (j->depth > 0) {
        j->depth--;
    }
}

static inline void rt_json_arr_open(rt_json *j, const char *key)
{
    rt_json_sep(j);
    if (key != NULL) {
        rt_json_printf(j, "\"%s\":[", key);
    } else {
        rt_json_raw(j, "[");
    }
    if (j->depth + 1 < RT_JSON_MAX_DEPTH) {
        j->depth++;
        j->sep[j->depth] = 0;
    }
}

static inline void rt_json_arr_close(rt_json *j)
{
    rt_json_raw(j, "]");
    if (j->depth > 0) {
        j->depth--;
    }
}

static inline void rt_json_num(rt_json *j, const char *key, double v, int prec)
{
    rt_json_sep(j);
    if (key != NULL) {
        rt_json_printf(j, "\"%s\":", key);
    }
    rt_json_printf(j, "%.*f", prec, v);
}

static inline void rt_json_u64(rt_json *j, const char *key, uint64_t v)
{
    rt_json_sep(j);
    if (key != NULL) {
        rt_json_printf(j, "\"%s\":", key);
    }
    rt_json_printf(j, "%llu", (unsigned long long)v);
}

static inline void rt_json_bool(rt_json *j, const char *key, int v)
{
    rt_json_sep(j);
    if (key != NULL) {
        rt_json_printf(j, "\"%s\":", key);
    }
    rt_json_raw(j, v ? "true" : "false");
}

static inline void rt_json_str(rt_json *j, const char *key, const char *s)
{
    rt_json_sep(j);
    if (key != NULL) {
        rt_json_printf(j, "\"%s\":", key);
    }
    rt_json_raw(j, "\"");
    for (; s != NULL && *s != '\0'; s++) {
        char c = *s;

        if (c == '"' || c == '\\') {
            char esc[3] = { '\\', c, '\0' };

            rt_json_raw(j, esc);
        } else if ((unsigned char)c < 0x20) {
            rt_json_printf(j, "\\u%04x", (unsigned)(unsigned char)c);
        } else {
            char one[2] = { c, '\0' };

            rt_json_raw(j, one);
        }
    }
    rt_json_raw(j, "\"");
}

/* ------------------------------------------------------------ statistics */

typedef struct {
    uint64_t count;
    uint64_t min;
    uint64_t max;
    double   mean;
    double   stddev;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
} rt_stats;

static inline int rt_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static inline uint64_t rt_percentile_sorted(const uint64_t *sorted, size_t n, double p)
{
    double   idx;
    size_t   i;

    if (n == 0) {
        return 0;
    }
    idx = p * (double)n / 100.0;
    i   = (size_t)idx;
    if ((double)i < idx) {
        i++;
    }
    if (i == 0) {
        i = 1;
    }
    if (i > n) {
        i = n;
    }
    return sorted[i - 1];
}

/* Sorts samples in place (ascending) and fills the statistics. */
static inline void rt_compute_stats(rt_stats *s, uint64_t *samples, size_t n)
{
    double sum = 0.0, mean, var = 0.0;
    size_t i;

    memset(s, 0, sizeof(*s));
    s->count = (uint64_t)n;
    if (n == 0) {
        return;
    }
    qsort(samples, n, sizeof(uint64_t), rt_cmp_u64);
    s->min = samples[0];
    s->max = samples[n - 1];
    for (i = 0; i < n; i++) {
        sum += (double)samples[i];
    }
    mean = sum / (double)n;
    s->mean = mean;
    for (i = 0; i < n; i++) {
        double d = (double)samples[i] - mean;

        var += d * d;
    }
    s->stddev = sqrt(var / (double)n);
    s->p50 = rt_percentile_sorted(samples, n, 50.0);
    s->p95 = rt_percentile_sorted(samples, n, 95.0);
    s->p99 = rt_percentile_sorted(samples, n, 99.0);
}

#endif /* RT_COMMON_H */

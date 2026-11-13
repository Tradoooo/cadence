/*
 * cadence_log.c — shared logging helpers for the Cadence QNX tools.
 */
#include "cadence_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void cadence_log(const char * level, const char * fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%lld.%03ld] %-9s ", (long long) ts.tv_sec, ts.tv_nsec / 1000000, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

double cadence_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1000000.0;
}

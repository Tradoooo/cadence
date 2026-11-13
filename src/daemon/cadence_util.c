/*
 * cadence_util.c — small helpers shared by the Cadence daemon.
 */
#define _QNX_SOURCE 1

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "cadence_util.h"

void cadence_log(const char * level, const char * fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%lld.%03ld] cadenced: %-5s ",
            (long long) ts.tv_sec, ts.tv_nsec / 1000000, level);
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

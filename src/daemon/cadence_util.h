/*
 * cadence_util.h — small helpers shared by the Cadence daemon.
 *
 * Kept in their own translation unit so that cadenced.c stays focused on the
 * message protocol and the inference loop.
 */
#ifndef CADENCE_UTIL_H
#define CADENCE_UTIL_H

/* Log on stderr with a timestamp. Level is a short string ("INFO", "WARN"...). */
void cadence_log(const char * level, const char * fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Monotonic clock, milliseconds. */
double cadence_now_ms(void);

#endif /* CADENCE_UTIL_H */

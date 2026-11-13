/*
 * cadence_log.h — shared logging helpers for the Cadence QNX tools.
 */
#ifndef CADENCE_LOG_H
#define CADENCE_LOG_H

/* Log a line on stderr with a timestamp and a level. */
void cadence_log(const char * level, const char * fmt, ...);

/* Monotonic clock, in milliseconds. */
double cadence_now_ms(void);

#endif /* CADENCE_LOG_H */

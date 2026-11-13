/*
 * rt_sleep.c -- guest side sleep.
 *
 * The QNX 8.0 image used for the proof has no sleep(1) utility (the toybox
 * applets of this image do not dispatch), so the campaign scripts use this
 * tiny helper instead of shelling out to sleep.
 *
 * Usage: rt_sleep <seconds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    struct timespec ts;
    double          seconds;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <seconds>\n", argv[0]);
        return 2;
    }
    seconds = atof(argv[1]);
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) {
        ts.tv_nsec = 0;
    }
    nanosleep(&ts, NULL);
    return 0;
}

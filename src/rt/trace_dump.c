/*
 * trace_dump.c -- copy a file from the guest RAM filesystem to a raw block
 * device, so the trace captured by tracelogger can be pulled out of the QNX
 * guest and analysed on the host with traceprinter.
 *
 * Usage: trace_dump <src_file> <dst_device> [skip_bytes]
 *
 * The destination is written sequentially from offset 0, which is what a raw
 * QNX disk device (/dev/hd1) expects.  The number of bytes copied is printed
 * on stdout.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 65536

int main(int argc, char **argv)
{
    const char *src;
    const char *dst;
    long        skip = 0;
    int         in, out;
    char       *buf;
    uint64_t    total = 0;
    int         b64mode = 0;

    if (argc >= 2 && strcmp(argv[1], "--base64") == 0) {
        b64mode = 1;
        dst     = "-";
        src = (argc > 2) ? argv[2] : NULL;
        if (src == NULL) {
            fprintf(stderr, "usage: %s --base64 <src_file>\n", argv[0]);
            return 2;
        }
    } else if (argc < 3) {
        fprintf(stderr, "usage: %s [--base64] <src_file> <dst_device|-> [skip_bytes]\n", argv[0]);
        return 2;
    } else {
        src = argv[1];
        dst = argv[2];
        if (argc > 3) {
            skip = strtol(argv[3], NULL, 10);
        }
    }

    in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "%s: open %s: %s\n", argv[0], src, strerror(errno));
        return 1;
    }
    if (strcmp(dst, "-") == 0) {
        out = STDOUT_FILENO;
    } else {
        out = open(dst, O_WRONLY | O_CREAT, 0666);
        if (out < 0) {
            fprintf(stderr, "%s: open %s: %s\n", argv[0], dst, strerror(errno));
            close(in);
            return 1;
        }
    }
    if (b64mode) {
        static const char b64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        unsigned char chunk[3];
        int          nchunk = 0;

        for (;;) {
            ssize_t n = read(in, chunk + nchunk, (size_t)(3 - nchunk));
            size_t  i;

            if (n <= 0) {
                break;
            }
            nchunk += (int)n;
            if (nchunk == 3) {
                putchar(b64[chunk[0] >> 2]);
                putchar(b64[((chunk[0] & 0x03) << 4) | (chunk[1] >> 4)]);
                putchar(b64[((chunk[1] & 0x0f) << 2) | (chunk[2] >> 6)]);
                putchar(b64[chunk[2] & 0x3f]);
                nchunk = 0;
                total += 3;
            }
        }
        if (nchunk == 1) {
            putchar(b64[chunk[0] >> 2]);
            putchar(b64[(chunk[0] & 0x03) << 4]);
            putchar('=');
            putchar('=');
            total += 1;
        } else if (nchunk == 2) {
            putchar(b64[chunk[0] >> 2]);
            putchar(b64[((chunk[0] & 0x03) << 4) | (chunk[1] >> 4)]);
            putchar(b64[(chunk[1] & 0x0f) << 2]);
            putchar('=');
            total += 2;
        }
        putchar('\n');
        fflush(stdout);
        fprintf(stderr, "%llu\n", (unsigned long long)total);
        close(in);
        return 0;
    }

    if (skip > 0 && lseek(in, skip, SEEK_SET) < 0) {
        fprintf(stderr, "%s: lseek: %s\n", argv[0], strerror(errno));
        close(in);
        close(out);
        return 1;
    }
    buf = malloc(CHUNK);
    if (buf == NULL) {
        close(in);
        close(out);
        return 1;
    }
    for (;;) {
        ssize_t n = read(in, buf, CHUNK);
        ssize_t off = 0;

        if (n < 0) {
            fprintf(stderr, "%s: read: %s\n", argv[0], strerror(errno));
            break;
        }
        if (n == 0) {
            break;
        }
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));

            if (w <= 0) {
                fprintf(stderr, "%s: write: %s\n", argv[0], strerror(errno));
                free(buf);
                close(in);
                close(out);
                return 1;
            }
            off += w;
        }
        total += (uint64_t)n;
    }
    free(buf);
    close(in);
    if (out != STDOUT_FILENO) {
        close(out);
    }
    fprintf(stderr, "%llu\n", (unsigned long long)total);
    return 0;
}

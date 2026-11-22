/*
 * dacplay: stream packed multi-bit DAC samples to N consecutive GPIOs
 * with the RP1 PIO, and report whether the PIO was kept fed.
 *
 *   dacplay FILE GPIO NPINS PULL_BITS DIV SECONDS [BLOCK_MB]
 *
 * FILE: little-endian uint32 words, each holding PULL_BITS/NPINS samples
 * LSB first (sample k in bits k*NPINS .. k*NPINS+NPINS-1). Looped until
 * SECONDS have been sent. Sample rate = 200 MHz / DIV.
 *
 * "speed" = signal time sent / wall time. 1.000 means no gaps; below
 * that the DMA could not keep up and the PIO ran dry.
 *
 * The rp1-pio driver's DMA call is synchronous, so the PIO runs dry for
 * ~0.1 ms between calls. DVB-T can't hide that (it exceeds the guard
 * interval), so by default the whole file is ONE call: the only gap is
 * at the loop seam. BLOCK_MB > 0 uses blocks of that size instead.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pio_out.h"

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    if (argc != 7 && argc != 8) {
        fprintf(stderr, "usage: dacplay FILE GPIO NPINS PULL_BITS DIV SECONDS [BLOCK_MB]\n");
        return 2;
    }
    const char *path = argv[1];
    int gpio = atoi(argv[2]), npins = atoi(argv[3]), pull = atoi(argv[4]);
    int div = atoi(argv[5]);
    double seconds = atof(argv[6]);
    double block_mb = argc > 7 ? atof(argv[7]) : 0;

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f) & ~3L;
    rewind(f);
    uint32_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 1; }
    fclose(f);
    size_t nwords = (size_t)len / 4;

    const double fs = 200e6 / div;
    const int spw = pull / npins;                 /* samples per word */
    const double words_per_s = fs / spw;
    printf("dacplay: %zu words (%.2f s), %d pins from GPIO %d, %d samples/word, "
           "%.3f MS/s, %.2f MB/s, %s\n", nwords, nwords / words_per_s, npins, gpio, spw,
           fs / 1e6, words_per_s * 4 / 1e6, block_mb > 0 ? "blocks" : "one DMA call per loop");

    pio_out_cfg_t pc = {
        .rf_gpio = gpio, .ptt_gpio = -1, .div = div, .drive = 3,
        .xfer_bytes = getenv("XFER_BYTES") ? (size_t)atol(getenv("XFER_BYTES"))
                    : (size_t)(getenv("XFER_KB") ? atoi(getenv("XFER_KB")) : 63) * 1024,
        .xfer_count = 4,
        .block_bytes = block_mb > 0 ? (size_t)(block_mb * 1048576) : (size_t)len,
        .npins = npins, .pull_bits = pull,
    };
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    if (pio_out_open(&pc) < 0)
        return 1;

    /* Speed: once the block queue is full, a pass of the loop can only be
     * written when one has been played, so loop wraps happen at the rate
     * the PIO consumes. Time the wraps after the start-up ones. */
    const size_t chunk = 1 << 16;                 /* words per write */
    const double loop_s = nwords / words_per_s;
    const size_t blk = block_mb > 0 ? (size_t)(block_mb * 1048576) : (size_t)len;
    const int skip = (int)(3.0 * blk / len) + 2;  /* wraps while the queue fills */
    size_t pos = 0;
    double sent = 0, t_first = 0, t_prev = 0;
    int wraps = 0, timed = 0;
    while (!g_stop && sent < seconds) {
        size_t n = nwords - pos < chunk ? nwords - pos : chunk;
        if (pio_out_write(buf + pos, n) < 0) { fprintf(stderr, "write failed\n"); break; }
        pos = (pos + n) % nwords;
        sent += n / words_per_s;
        if (pos == 0) {
            double t = now();
            if (++wraps == skip) {
                t_first = t;
            } else if (wraps > skip) {
                timed++;
                printf("  loop %3d: %.6f s per pass (x%.5f), overall x%.5f\n", wraps,
                       t - t_prev, loop_s / (t - t_prev), timed * loop_s / (t - t_first));
                fflush(stdout);
            }
            t_prev = t;
        }
    }
    if (timed)
        printf("dacplay: speed x%.5f over %d passes (%.1f s); lost %.1f us per pass\n",
               timed * loop_s / (t_prev - t_first), timed, t_prev - t_first,
               ((t_prev - t_first) / timed - loop_s) * 1e6);
    pio_out_close();
    free(buf);
    return 0;
}

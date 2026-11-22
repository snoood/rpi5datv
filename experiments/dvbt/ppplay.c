/*
 * ppplay: gap-free multi-bit DAC playback with two ping-pong PIO state
 * machines (RP1 / Raspberry Pi 5).
 *
 *   ppplay FILE GPIO NPINS PULL_BITS DIV TURN_WORDS SECONDS [CAPFILE CAP_MB]
 *
 * Why: the rp1-pio driver's DMA stalls ~16 us between transactions
 * (bounce buffers); with one state machine that stall stops the output,
 * which DVB-T can't tolerate. Here two SMs drive the same pins and take
 * turns of N samples (TURN_WORDS words, also the DMA transaction size),
 * so each SM's DMA stall falls in its idle half.
 *
 * How (no IRQs, no exec, nothing that can hang the RP1 firmware):
 *   - The PIO runs at twice the sample rate; every sample is 2 cycles.
 *   - Both SMs run the same fixed 4N-cycle schedule: play N samples
 *     (2N cycles), idle 2N cycles. SM0 starts with "play", SM1 with
 *     "idle", so SM1's first sample comes exactly 2 cycles after SM0's
 *     last, and so on, forever, as long as neither SM ever stalls.
 *   - Start: both prologues load Y (turn length) from the FIFO and wait
 *     for a "go" GPIO, raised by a third tiny SM when we put a word into
 *     its FIFO, after the DMA has filled both FIFOs. So both leave the
 *     wait in the same cycle with data ready.
 *   - Only non-blocking FIFO accesses are used: a blocking put/get to a
 *     stalled SM hangs the RP1 firmware until reboot. Before any DMA is
 *     started, both SMs are checked to have taken Y (i.e. are running);
 *     DMA to an SM that doesn't drain also wedges the RP1 DMA.
 *
 * FILE: packed words (PULL_BITS/NPINS samples each, LSB first); an even
 * number of turns. Sample rate = 200 MHz / DIV (DIV even). With CAPFILE,
 * a fourth SM samples the lowest output pin every PIO cycle (DMA) and
 * CAP_MB of it is saved, for pp_check.py.
 *
 * Env: PP_GO_GPIO (default 27). Debug: PP_SLOW=1 feeds the FIFOs from the
 * CPU (timing tests at very low rates, e.g. DIV 131070); PP_CAP_BITS,
 * PP_CAP_EVERY (capture width / every N cycles); PP_NOFEED=1 (prefill two
 * turns, no feeder); PP_ONLY=0|1 (run one output SM); PP_SM1_PLAY=1,
 * PP_SWAP=1 (start/slot swaps); PP_XFER_COUNT; PP_DEBUG=1 (print programs).
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "piolib.h"

static PIO pio;
static int g_slow;
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Non-blocking put with a deadline; 0 = ok. */
static int put_nb(uint sm, uint32_t v, double timeout_s)
{
    double end = now() + timeout_s;
    while (pio_sm_is_tx_fifo_full(pio, sm)) {
        if (g_stop || now() > end)
            return -1;
        usleep(50);
    }
    pio_sm_put(pio, sm, v);
    return 0;
}

typedef struct {
    int sm;
    uint8_t *data;        /* this SM's turns, back to back */
    size_t bytes;
    double seconds;
    double pass_s;        /* duration of one pass of the whole file */
    int err;
    long passes;
} writer_t;

static void *writer(void *arg)
{
    writer_t *w = arg;
    struct sched_param sp = { .sched_priority = 60 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    double sent = 0;
    while (!g_stop && sent < w->seconds) {
        if (g_slow) {
            const uint32_t *p = (const uint32_t *)w->data;
            for (size_t i = 0; i < w->bytes / 4; i++)
                if (put_nb((uint)w->sm, p[i], 2.0)) { w->err = -ETIMEDOUT; return NULL; }
        } else {
            int r = pio_sm_xfer_data(pio, (uint)w->sm, PIO_DIR_TO_SM, (uint)w->bytes, w->data);
            if (r) { w->err = r; break; }
        }
        sent += w->pass_s;
        w->passes++;
    }
    return NULL;
}

typedef struct { int sm; uint8_t *buf; size_t bytes; int err; } capture_t;

static void *capturer(void *arg)
{
    capture_t *c = arg;
    if (g_slow) {
        uint32_t *p = (uint32_t *)c->buf;
        for (size_t i = 0; i < c->bytes / 4; i++) {
            double end = now() + 2.0;
            while (pio_sm_get_rx_fifo_level(pio, (uint)c->sm) == 0) {
                if (now() > end) { c->err = -ETIMEDOUT; return NULL; }
                usleep(50);
            }
            p[i] = pio_sm_get(pio, (uint)c->sm);
        }
        return NULL;
    }
    c->err = pio_sm_xfer_data(pio, (uint)c->sm, PIO_DIR_FROM_SM, (uint)c->bytes, c->buf);
    return NULL;
}

/* Add a program at a fixed offset. Jump targets are written relative to
 * the program start: piolib (like the Pico SDK) adds the load offset to
 * every JMP itself. (Patching in absolute addresses double-relocated
 * them; harmless only at offset 0, which hid it.) Returns offset or -1. */
static int add_prog(uint16_t *ins, int n, uint off, void (*patch)(uint16_t *, uint, void *), void *ctx)
{
    if (patch)
        patch(ins, 0, ctx);
    pio_program_t prog = { .instructions = ins, .length = (uint8_t)n, .origin = (int8_t)off };
    if (!pio_can_add_program_at_offset(pio, &prog, off))
        return -1;
    pio_add_program_at_offset(pio, &prog, off);
    return (int)off;
}

/* Output program layout (relative):
 *   0 out y, 32           prologue: Y = N - 2 (first FIFO word, via autopull)
 *   1 wait 1 gpio GO
 *   2 jmp PLAY (SM0) / IDLE (SM1)
 *   3 PLAY: out pins, n         wrap_target   (cycle 0: sample 1)
 *   4       mov x, y                          (1)
 *   5 LOOP: out pins, n                       (2, 4, ... 2N-2: samples 2..N)
 *   6       jmp x-- LOOP                      (... 2N-1)
 *   7 IDLE: mov x, y                          (idle cycle 0)
 *   8 ILP:  jmp x-- ILP [1]                   (2 cycles x (N-1))
 *   9       nop                     wrap      (idle cycle 2N-1)
 * (pull + mov y, osr left Y = 0 on the RP1 with autopull on.)
 */
#define P_PLAY 3
#define P_LOOP 5
#define P_IDLE 7
#define P_ILP  8
#define P_LEN  10
struct outctx { int start_idle; };
static void patch_out(uint16_t *ins, uint off, void *vctx)
{
    struct outctx *c = vctx;
    ins[2] = (uint16_t)pio_encode_jmp(off + (c->start_idle ? P_IDLE : P_PLAY));
    ins[6] = (uint16_t)pio_encode_jmp_x_dec(off + P_LOOP);
    ins[P_ILP] = (uint16_t)(pio_encode_jmp_x_dec(off + P_ILP) | pio_encode_delay(1));
}

static int load_out(int npins, int start_idle, uint go, uint at)
{
    uint16_t ins[P_LEN];
    ins[0] = (uint16_t)pio_encode_out(pio_y, 32);
    ins[1] = (uint16_t)pio_encode_wait_gpio(true, go);
    ins[2] = 0;
    ins[3] = (uint16_t)pio_encode_out(pio_pins, (uint)npins);
    ins[4] = (uint16_t)pio_encode_mov(pio_x, pio_y);
    ins[5] = (uint16_t)pio_encode_out(pio_pins, (uint)npins);
    ins[6] = 0;
    ins[7] = (uint16_t)pio_encode_mov(pio_x, pio_y);
    ins[8] = 0;
    ins[9] = (uint16_t)pio_encode_nop();
    struct outctx c = { start_idle };
    int off = add_prog(ins, P_LEN, at, patch_out, &c);
    if (getenv("PP_DEBUG")) {
        printf("  out program (start %s) at %d:", start_idle ? "idle" : "play", off);
        for (int i = 0; i < P_LEN; i++) printf(" %04x", ins[i]);
        printf("\n");
    }
    return off;
}

/* go program: wait for a FIFO word, raise the go pin, then halt. */
static void patch_go(uint16_t *ins, uint off, void *ctx)
{
    (void)ctx;
    ins[2] = (uint16_t)pio_encode_jmp(off + 2);
}

int main(int argc, char **argv)
{
    if (argc != 8 && argc != 10) {
        fprintf(stderr, "usage: ppplay FILE GPIO NPINS PULL_BITS DIV TURN_WORDS SECONDS [CAPFILE CAP_MB]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argv[1];
    int gpio = atoi(argv[2]), npins = atoi(argv[3]), pull = atoi(argv[4]);
    int div = atoi(argv[5]);
    size_t turn_words = (size_t)atol(argv[6]);
    double seconds = atof(argv[7]);
    const char *capfile = argc > 8 ? argv[8] : NULL;
    size_t cap_bytes = argc > 8 ? (size_t)(atof(argv[9]) * 1048576) & ~(size_t)3 : 0;
    g_slow = getenv("PP_SLOW") && atoi(getenv("PP_SLOW"));
    uint go = getenv("PP_GO_GPIO") ? (uint)atoi(getenv("PP_GO_GPIO")) : 27;

    if (div % 2 || pull % npins || pull > 32) {
        fprintf(stderr, "DIV must be even; PULL_BITS a multiple of NPINS\n");
        return 2;
    }
    if (go >= (uint)gpio && go < (uint)(gpio + npins)) {
        fprintf(stderr, "go GPIO %u overlaps the output pins\n", go);
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    size_t nwords = (size_t)ftell(f) / 4;
    rewind(f);
    uint32_t *all = malloc(nwords * 4);
    if (!all || fread(all, 4, nwords, f) != nwords) { perror("read"); return 1; }
    fclose(f);
    size_t turns = nwords / turn_words;
    if (turns * turn_words != nwords || turns % 2) {
        fprintf(stderr, "file (%zu words) must be an even number of %zu-word turns\n",
                nwords, turn_words);
        return 2;
    }
    const int spw = pull / npins;
    const double fs = 200e6 / div;
    const uint32_t n_samples = (uint32_t)(turn_words * (size_t)spw);
    const double pass_s = nwords * (double)spw / fs;
    size_t half = nwords / 2;
    uint32_t *st[2] = { malloc(half * 4), malloc(half * 4) };
    for (size_t t = 0; t < turns; t++)
        memcpy(st[t % 2] + (t / 2) * turn_words, all + t * turn_words, turn_words * 4);
    free(all);
    printf("ppplay: %zu words = %.3f s, %zu turns of %zu words (%u samples, %.3f ms), "
           "%d pins from GPIO %d, %.3f MS/s, %.2f MB/s%s\n", nwords, pass_s, turns, turn_words,
           n_samples, n_samples / fs * 1e3, npins, gpio, fs / 1e6, fs / spw * 4 / 1e6,
           g_slow ? " (slow: CPU-fed)" : "");

    pio = pio_open(0);
    if (PIO_IS_ERR(pio)) { fprintf(stderr, "cannot open PIO\n"); return 1; }
    pio_select(pio);
    pio_enable_fatal_errors(pio, false);
    const int nsm = capfile ? 4 : 3;
    int sm[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < nsm; i++) {
        sm[i] = pio_claim_unused_sm(pio, false);
        if (sm[i] < 0) { fprintf(stderr, "need %d free state machines\n", nsm); return 1; }
    }
    int sm1_idle = (getenv("PP_SM1_PLAY") && atoi(getenv("PP_SM1_PLAY"))) ? 0 : 1;      /* debug: SM1 starts in PLAY */
    int swap = getenv("PP_SWAP") && atoi(getenv("PP_SWAP"));   /* debug: swap program slots */
    int off0 = load_out(npins, 0, go, swap ? P_LEN : 0);
    int off1 = load_out(npins, sm1_idle, go, swap ? 0 : P_LEN);
    uint16_t go_ins[3] = { (uint16_t)pio_encode_pull(false, true),
                           (uint16_t)pio_encode_set(pio_pins, 1), 0 };
    int offg = add_prog(go_ins, 3, 2 * P_LEN, patch_go, NULL);
    int cap_bits = getenv("PP_CAP_BITS") ? atoi(getenv("PP_CAP_BITS")) : 1;
    /* PP_CAP_EVERY=2: sample once per output sample (half the capture
     * bandwidth), at the second cycle of each sample */
    int cap_every = getenv("PP_CAP_EVERY") ? atoi(getenv("PP_CAP_EVERY")) : 1;
    uint16_t cap_ins[1] = { (uint16_t)(pio_encode_in(pio_pins, (uint)cap_bits) |
                                       pio_encode_delay((uint)(cap_every - 1))) };
    int offc = capfile ? add_prog(cap_ins, 1, 2 * P_LEN + 3, NULL, NULL) : 0;
    if (off0 < 0 || off1 < 0 || offg < 0 || offc < 0) { fprintf(stderr, "no room for PIO programs\n"); return 1; }
    const int off[2] = { off0, off1 };

    for (int i = 0; i < npins; i++) {
        pio_gpio_init(pio, (uint)(gpio + i));
        gpio_set_drive_strength((uint)(gpio + i), GPIO_DRIVE_STRENGTH_12MA);
    }
    pio_gpio_init(pio, go);
    /* Pin values/directions BEFORE pio_sm_init: on the RP1 these helpers
     * disturb the SM they run on (FIFO flushed, config reset). */
    pio_sm_set_pins_with_mask(pio, (uint)sm[0], 0, (((1u << npins) - 1) << gpio) | (1u << go));
    pio_sm_set_pindirs_with_mask(pio, (uint)sm[0], (((1u << npins) - 1) << gpio) | (1u << go),
                                 (((1u << npins) - 1) << gpio) | (1u << go));
    for (int k = 0; k < 2; k++) {
        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_out_pins(&c, (uint)gpio, (uint)npins);
        sm_config_set_wrap(&c, (uint)off[k] + P_PLAY, (uint)off[k] + P_LEN - 1);
        sm_config_set_out_shift(&c, true, true, (uint)pull);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        sm_config_set_clkdiv_int_frac(&c, (uint16_t)(div / 2), 0);
        pio_sm_init(pio, (uint)sm[k], (uint)off[k], &c);
        int r = pio_sm_config_xfer(pio, (uint)sm[k], PIO_DIR_TO_SM,
                                   (uint)(g_slow ? 4096 : turn_words * 4),
                                   getenv("PP_XFER_COUNT") ? (uint)atoi(getenv("PP_XFER_COUNT")) : 4);
        if (r) { fprintf(stderr, "config_xfer(SM%d): %s\n", sm[k], strerror(-r)); return 1; }
        pio_sm_clear_fifos(pio, (uint)sm[k]);
    }
    {   /* go SM: owns the go pin, low until released */
        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_set_pins(&c, go, 1);
        sm_config_set_wrap(&c, (uint)offg, (uint)offg + 2);
        sm_config_set_clkdiv_int_frac(&c, (uint16_t)(div / 2), 0);
        pio_sm_init(pio, (uint)sm[2], (uint)offg, &c);
        pio_sm_config_xfer(pio, (uint)sm[2], PIO_DIR_TO_SM, 4096, 2);
        pio_sm_clear_fifos(pio, (uint)sm[2]);
    }
    if (capfile) {
        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_in_pins(&c, (uint)gpio);
        sm_config_set_wrap(&c, (uint)offc, (uint)offc);
        sm_config_set_in_shift(&c, true, true, (uint)(32 / cap_bits * cap_bits));
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
        sm_config_set_clkdiv_int_frac(&c, (uint16_t)(div / 2), 0);
        pio_sm_init(pio, (uint)sm[3], (uint)offc, &c);
        int r = pio_sm_config_xfer(pio, (uint)sm[3], PIO_DIR_FROM_SM,
                                   (uint)(g_slow ? 4096 : cap_bytes), 1);
        if (r) { fprintf(stderr, "config_xfer(capture): %s\n", strerror(-r)); return 1; }
        pio_sm_clear_fifos(pio, (uint)sm[3]);
        /* (DMACTRL can't be read on this kernel: GET_DMACTRL ioctl missing.
         * The capture's RX DMA returns repeated/zero words when the RX FIFO
         * isn't kept well filled — capture artefacts, not output errors.) */
    }
    /* Y last: other set-up calls (pin set-up on an SM) can reset its FIFO */
    for (int k = 0; k < 2; k++)
        put_nb((uint)sm[k], n_samples - 2, 0.1);           /* prologue: Y = N - 2 */
    unsigned b0 = pio_sm_get_tx_fifo_level(pio, (uint)sm[0]);
    unsigned b1 = pio_sm_get_tx_fifo_level(pio, (uint)sm[1]);
    printf("  FIFO before enable: SM0 %u, SM1 %u (expect 1 each)\n", b0, b1);

    uint32_t mask = 0;
    for (int i = 0; i < nsm; i++)
        mask |= 1u << sm[i];
    if (getenv("PP_ONLY"))                 /* debug: only one output SM */
        mask &= ~(1u << sm[1 - atoi(getenv("PP_ONLY"))]);
    pio_enable_sm_in_sync_mask(pio, mask);
    usleep(10000);
    unsigned l0 = pio_sm_get_tx_fifo_level(pio, (uint)sm[0]);
    unsigned l1 = pio_sm_get_tx_fifo_level(pio, (uint)sm[1]);
    printf("  FIFO after enable:  SM0 %u, SM1 %u (expect 0: Y taken, waiting for go)\n", l0, l1);
    int ok = (b0 == 1 && b1 == 1 && l0 == 0 && l1 == 0);
    if (getenv("PP_ONLY"))
        ok = (atoi(getenv("PP_ONLY")) ? l1 : l0) == 0;
    if (!ok)
        fprintf(stderr, "pre-flight failed: state machines not running; aborting before DMA\n");

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    writer_t w[2];
    pthread_t th[2], cth;
    capture_t cap = { .sm = sm[3] };
    int started = 0;
    int nofeed = getenv("PP_NOFEED") && atoi(getenv("PP_NOFEED"));
    if (ok && nofeed) {            /* debug: prefill 2 turns per SM, no feeder */
        for (int k = 0; k < 2; k++)
            for (size_t i = 0; i < 2 * turn_words && i < half; i++)
                put_nb((uint)sm[k], st[k][i], 0.1);
        printf("  prefilled: SM0 %u, SM1 %u words; sm ids %d %d %d %d\n",
               pio_sm_get_tx_fifo_level(pio, (uint)sm[0]), pio_sm_get_tx_fifo_level(pio, (uint)sm[1]),
               sm[0], sm[1], sm[2], sm[3]);
        if (capfile) {
            cap.buf = malloc(cap_bytes);
            cap.bytes = cap_bytes;
            pthread_create(&cth, NULL, capturer, &cap);
            usleep(5000);
        }
        put_nb((uint)sm[2], 1, 0.1);
        if (capfile) {
            pthread_join(cth, NULL);
            FILE *o = fopen(capfile, "wb");
            fwrite(cap.buf, 1, cap.bytes, o);
            fclose(o);
        }
        printf("  after: SM0 %u, SM1 %u words left\n",
               pio_sm_get_tx_fifo_level(pio, (uint)sm[0]), pio_sm_get_tx_fifo_level(pio, (uint)sm[1]));
        ok = 0;
    }
    if (ok) {
        for (int k = 0; k < 2; k++) {
            w[k] = (writer_t){ .sm = sm[k], .data = (uint8_t *)st[k], .bytes = half * 4,
                               .seconds = seconds, .pass_s = pass_s };
            pthread_create(&th[k], NULL, writer, &w[k]);
        }
        started = 1;
        /* wait until both FIFOs have data, then go. Don't linger: DMA to an
         * SM that isn't draining times out in the driver after ~1 s (and the
         * FIFO only ever reports 7 here, never 8). */
        double end = now() + 0.2;
        while ((pio_sm_get_tx_fifo_level(pio, (uint)sm[0]) < 4 ||
                pio_sm_get_tx_fifo_level(pio, (uint)sm[1]) < 4) && now() < end)
            usleep(200);
        printf("  FIFO before go:     SM0 %u, SM1 %u (expect >= 4)\n",
               pio_sm_get_tx_fifo_level(pio, (uint)sm[0]), pio_sm_get_tx_fifo_level(pio, (uint)sm[1]));
        if (capfile) {
            cap.buf = malloc(cap_bytes);
            cap.bytes = cap_bytes;
            pthread_create(&cth, NULL, capturer, &cap);
            usleep(5000);
        }
        put_nb((uint)sm[2], 1, 0.1);                       /* go */
    }

    double t0 = now(), t_first = 0;
    long last_p = -1, p_first = 0;
    while (ok && !g_stop) {
        usleep(100000);
        long p = w[0].passes < w[1].passes ? w[0].passes : w[1].passes;
        if (p != last_p) {
            double t = now();
            if (p == 2) { t_first = t; p_first = p; }
            else if (p > 2 && t_first)
                printf("  pass %ld: overall x%.6f\n", p, (p - p_first) * pass_s / (t - t_first));
            last_p = p;
        }
        if (w[0].err || w[1].err) { fprintf(stderr, "feed error %d/%d\n", w[0].err, w[1].err); break; }
        if (w[0].passes * pass_s >= seconds && w[1].passes * pass_s >= seconds) break;
        if (now() - t0 > seconds * 3 + 10) { fprintf(stderr, "timeout\n"); break; }
    }
    g_stop = 1;
    if (started) {
        if (capfile) {
            pthread_join(cth, NULL);
            FILE *o = fopen(capfile, "wb");
            fwrite(cap.buf, 1, cap.bytes, o);
            fclose(o);
            printf("capture: %zu bytes -> %s (err %d)\n", cap.bytes, capfile, cap.err);
        }
        for (int k = 0; k < 2; k++)
            pthread_join(th[k], NULL);
    }
    pio_set_sm_mask_enabled(pio, mask, false);
    pio_sm_set_pins_with_mask(pio, (uint)sm[0], 0, ((1u << npins) - 1) << gpio);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm[0], (uint)gpio, (uint)npins, false);
    pio_sm_set_pindirs_with_mask(pio, (uint)sm[2], 0, 1u << go);
    pio_close(pio);
    printf("ppplay: done (%ld/%ld passes)\n", started ? w[0].passes : 0, started ? w[1].passes : 0);
    return ok ? 0 : 1;
}

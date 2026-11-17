/*
 * rpi5datv - RP1 PIO output, see pio_out.h
 */
#include "pio_out.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "piolib.h"

/* pioasm: "out pins, 1" = 0x6001 (OUT, dest PINS, bit count 1) */
static const uint16_t rf_program_instructions[] = { 0x6001 };
static const pio_program_t rf_program = {
    .instructions = rf_program_instructions,
    .length = 1,
    .origin = -1,
};

static PIO g_pio;
static int g_sm = -1;
static int g_offset = -1;
static pio_out_cfg_t g_cfg;

/*
 * Block queue between the modulator (pio_out_write) and the writer thread.
 *
 * The rp1-pio driver's pio_sm_xfer_data() is synchronous: inside one call
 * it keeps its bounce buffers busy back to back, but it only returns once
 * all the DMA has finished, so the 8-word TX FIFO runs dry until the next
 * call has copied its first buffer. Calling it once per small buffer from
 * the modulator loop left the PIO idle ~7% of the time (measured on a
 * Pi 500, independent of the sample rate). So a dedicated thread does
 * nothing but issue large calls (one block, ~1 MiB) back to back, while
 * the modulator fills the next block in parallel.
 */
#define NBLOCKS 3
static uint8_t *g_block[NBLOCKS];
static size_t g_block_len[NBLOCKS];
static size_t g_block_size;          /* bytes per block (multiple of 4) */
static size_t g_call_max;            /* max bytes per pio_sm_xfer_data call */
static int g_head, g_tail, g_queued; /* filled blocks: g_tail..g_head */
static size_t g_fill;                /* bytes in block g_head being filled */
static int g_writer_err;
static int g_writer_stop;
static int g_writer_running;
static pthread_t g_writer;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static int xfer(const uint8_t *p, size_t bytes)
{
    while (bytes) {
        size_t chunk = bytes < g_call_max ? bytes : g_call_max;
        int ret = pio_sm_xfer_data(g_pio, (uint)g_sm, PIO_DIR_TO_SM,
                                   (uint)chunk, (void *)p);
        if (ret && chunk > 0xfffc) {
            /* Kernel without the 32-bit xfer ioctl: use 16-bit calls. */
            fprintf(stderr, "PIO: large DMA calls not supported (%s); "
                            "using 64 KiB calls (expect small gaps)\n",
                    strerror(ret < 0 ? -ret : ret));
            g_call_max = 0xfffc;
            continue;
        }
        if (ret)
            return ret < 0 ? ret : -ret;
        p += chunk;
        bytes -= chunk;
    }
    return 0;
}

static void *writer_thread(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 60 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   /* best effort */

    pthread_mutex_lock(&g_mx);
    for (;;) {
        while (!g_queued && !g_writer_stop)
            pthread_cond_wait(&g_cv, &g_mx);
        if (!g_queued)
            break;                       /* stopping and nothing left */
        int i = g_tail;
        pthread_mutex_unlock(&g_mx);

        int e = g_writer_stop ? 0 : xfer(g_block[i], g_block_len[i]);

        pthread_mutex_lock(&g_mx);
        g_tail = (g_tail + 1) % NBLOCKS;
        g_queued--;
        if (e && !g_writer_err)
            g_writer_err = e;
        pthread_cond_broadcast(&g_cv);
        if (e)
            break;
    }
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

int pio_out_open(const pio_out_cfg_t *cfg)
{
    g_cfg = *cfg;

    if (cfg->rf_gpio < 0 || cfg->rf_gpio > 27) {
        fprintf(stderr, "RF GPIO must be 0..27 (header GPIOs)\n");
        return -1;
    }
    if (cfg->ptt_gpio == cfg->rf_gpio) {
        fprintf(stderr, "PTT GPIO must differ from the RF GPIO\n");
        return -1;
    }

    g_pio = pio_open(0);
    if (PIO_IS_ERR(g_pio)) {
        fprintf(stderr,
                "Cannot open /dev/pio0 (error %d).\n"
                "  - Is this a Raspberry Pi 5/500 with an up-to-date kernel and EEPROM?\n"
                "  - Are you in the 'gpio' group (or running as root)?\n"
                "  Run ./install.sh to set this up.\n",
                PIO_ERR_VAL(g_pio));
        g_pio = NULL;
        return -1;
    }
    pio_select(g_pio);
    pio_enable_fatal_errors(g_pio, false);

    g_sm = pio_claim_unused_sm(g_pio, false);
    if (g_sm < 0) {
        fprintf(stderr, "No free PIO state machine\n");
        pio_out_close();
        return -1;
    }
    if (!pio_can_add_program(g_pio, &rf_program)) {
        fprintf(stderr, "No room in PIO instruction memory\n");
        pio_out_close();
        return -1;
    }
    g_offset = (int)pio_add_program(g_pio, &rf_program);

    /* Set up the DMA buffers. Sizes above 0xffff bytes need the newer
     * *_XFER32 ioctls, which older rp1-pio kernel drivers lack, so fall
     * back to smaller buffers (and fewer of them) until the driver
     * accepts the configuration. */
    {
        size_t bytes = cfg->xfer_bytes & ~(size_t)3;
        int count = cfg->xfer_count;
        int ret = -1;
        for (;;) {
            ret = pio_sm_config_xfer(g_pio, g_sm, PIO_DIR_TO_SM,
                                     (uint)bytes, (uint)count);
            if (ret == 0)
                break;
            fprintf(stderr, "pio_sm_config_xfer(%zu bytes x %d) failed: %s\n",
                    bytes, count, strerror(ret < 0 ? -ret : ret));
            /* Fewer buffers first (the Pi 5 driver accepts at most 4),
             * then smaller ones: big buffers mean fewer ioctls per second. */
            if (bytes > 0xfffc)
                bytes = 0xfffc & ~(size_t)3;     /* largest 16-bit size */
            else if (count > 4)
                count = 4;
            else if (bytes > 4096)
                bytes /= 2;
            else if (count > 2)
                count /= 2;
            else
                break;
            bytes &= ~(size_t)3;
        }
        if (ret != 0) {
            fprintf(stderr, "Cannot set up PIO DMA; is the kernel up to date "
                            "(sudo apt full-upgrade)?\n");
            pio_out_close();
            return -1;
        }
        if (bytes != cfg->xfer_bytes || count != cfg->xfer_count)
            fprintf(stderr, "PIO DMA: using %zu-byte buffers x %d\n", bytes, count);
        g_cfg.xfer_bytes = bytes;
        g_cfg.xfer_count = count;
    }

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_out_pins(&c, (uint)cfg->rf_gpio, 1);
    sm_config_set_wrap(&c, (uint)g_offset, (uint)g_offset);
    sm_config_set_out_shift(&c, true /* LSB first */, true /* autopull */, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac(&c, (uint16_t)cfg->div, 0);

    pio_gpio_init(g_pio, (uint)cfg->rf_gpio);
    gpio_set_drive_strength((uint)cfg->rf_gpio, (enum gpio_drive_strength)(cfg->drive & 3));
    pio_sm_set_pins_with_mask(g_pio, (uint)g_sm, 0, 1u << cfg->rf_gpio);
    pio_sm_set_consecutive_pindirs(g_pio, (uint)g_sm, (uint)cfg->rf_gpio, 1, true);

    if (cfg->ptt_gpio >= 0) {
        pio_gpio_init(g_pio, (uint)cfg->ptt_gpio);
        pio_sm_set_pins_with_mask(g_pio, (uint)g_sm, 1u << cfg->ptt_gpio,
                                  1u << cfg->ptt_gpio);
        pio_sm_set_pindirs_with_mask(g_pio, (uint)g_sm, 1u << cfg->ptt_gpio,
                                     1u << cfg->ptt_gpio);
    }

    pio_sm_init(g_pio, (uint)g_sm, (uint)g_offset, &c);
    pio_sm_clear_fifos(g_pio, (uint)g_sm);
    pio_sm_set_enabled(g_pio, (uint)g_sm, true);

    if (pio_get_error(g_pio)) {
        fprintf(stderr, "PIO setup reported an error\n");
        pio_out_close();
        return -1;
    }

    g_block_size = (cfg->block_bytes ? cfg->block_bytes : (size_t)1 << 20) & ~(size_t)3;
    if (g_block_size < g_cfg.xfer_bytes)
        g_block_size = g_cfg.xfer_bytes;
    g_call_max = g_block_size;
    for (int i = 0; i < NBLOCKS; i++) {
        g_block[i] = malloc(g_block_size);
        if (!g_block[i]) {
            fprintf(stderr, "out of memory for PIO blocks\n");
            pio_out_close();
            return -1;
        }
    }
    g_head = g_tail = g_queued = 0;
    g_fill = 0;
    g_writer_err = 0;
    g_writer_stop = 0;
    if (pthread_create(&g_writer, NULL, writer_thread, NULL) != 0) {
        fprintf(stderr, "cannot start PIO writer thread\n");
        pio_out_close();
        return -1;
    }
    g_writer_running = 1;
    return 0;
}

int pio_out_write(const uint32_t *words, size_t nwords)
{
    const uint8_t *p = (const uint8_t *)words;
    size_t bytes = nwords * 4;
    while (bytes) {
        size_t n = g_block_size - g_fill;
        if (n > bytes)
            n = bytes;
        memcpy(g_block[g_head] + g_fill, p, n);
        g_fill += n;
        p += n;
        bytes -= n;
        if (g_fill < g_block_size)
            break;

        /* Block full: hand it to the writer, wait for a free one. */
        pthread_mutex_lock(&g_mx);
        g_block_len[g_head] = g_fill;
        g_head = (g_head + 1) % NBLOCKS;
        g_queued++;
        pthread_cond_broadcast(&g_cv);
        while (g_queued == NBLOCKS && !g_writer_err)
            pthread_cond_wait(&g_cv, &g_mx);
        int e = g_writer_err;
        pthread_mutex_unlock(&g_mx);
        g_fill = 0;
        if (e)
            return e;
    }
    return 0;
}

void pio_out_close(void)
{
    if (!g_pio)
        return;
    if (g_writer_running) {
        /* Stop now (do not drain): the caller is shutting down. A call
         * already in progress finishes within one block (~0.1 s). */
        pthread_mutex_lock(&g_mx);
        g_writer_stop = 1;
        g_queued = g_queued ? 1 : 0;     /* drop queued blocks */
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mx);
        pthread_join(g_writer, NULL);
        g_writer_running = 0;
    }
    for (int i = 0; i < NBLOCKS; i++) {
        free(g_block[i]);
        g_block[i] = NULL;
    }
    if (g_sm >= 0) {
        pio_sm_set_enabled(g_pio, (uint)g_sm, false);
        /* RF pin low then high impedance; PTT off */
        pio_sm_set_pins_with_mask(g_pio, (uint)g_sm, 0, 1u << g_cfg.rf_gpio);
        pio_sm_set_consecutive_pindirs(g_pio, (uint)g_sm, (uint)g_cfg.rf_gpio, 1, false);
        if (g_cfg.ptt_gpio >= 0) {
            pio_sm_set_pins_with_mask(g_pio, (uint)g_sm, 0, 1u << g_cfg.ptt_gpio);
        }
        if (g_offset >= 0)
            pio_remove_program(g_pio, &rf_program, (uint)g_offset);
        pio_sm_unclaim(g_pio, (uint)g_sm);
    }
    pio_close(g_pio);
    g_pio = NULL;
    g_sm = -1;
    g_offset = -1;
}

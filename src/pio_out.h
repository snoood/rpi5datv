/*
 * rpi5datv - RP1 PIO output (Raspberry Pi 5 / 500)
 *
 * One PIO state machine running a single instruction, "out pins, 1",
 * with autopull: every PIO clock (200 MHz / div) one bit of the DDS
 * bitstream is placed on the RF GPIO. Data is streamed to the TX FIFO
 * with the rp1-pio kernel driver's DMA (piolib pio_sm_xfer_data), from
 * a dedicated writer thread issuing large calls back to back.
 */
#ifndef RPI5DATV_PIO_OUT_H
#define RPI5DATV_PIO_OUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int rf_gpio;
    int ptt_gpio;          /* -1 = none */
    int div;               /* integer PIO clock divider */
    int drive;             /* 0..3 = 2/4/8/12 mA */
    size_t xfer_bytes;     /* DMA buffer size */
    int xfer_count;        /* number of DMA buffers */
    size_t block_bytes;    /* bytes per pio_sm_xfer_data call (0 = 1 MiB) */
} pio_out_cfg_t;

int pio_out_open(const pio_out_cfg_t *cfg);
/* Blocking write; nwords*4 bytes. Returns 0 or negative error. */
int pio_out_write(const uint32_t *words, size_t nwords);
void pio_out_close(void);

#endif

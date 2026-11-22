/*
 * captest: check the ppplay capture path on its own. One SM toggles a
 * GPIO every 2 PIO cycles (no DMA, so it can't glitch); another samples
 * it every cycle into RX DMA, exactly as ppplay's capture does.
 *
 *   captest GPIO DIV CAP_MB OUTFILE [SAMPLE_EVERY]
 *
 * SAMPLE_EVERY = 1 (every PIO cycle) or 2 (every other cycle).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "piolib.h"

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: captest GPIO DIV CAP_MB OUTFILE [SAMPLE_EVERY]\n"); return 2; }
    int gpio = atoi(argv[1]), div = atoi(argv[2]);
    size_t bytes = (size_t)(atof(argv[3]) * 1048576) & ~(size_t)3;
    int every = argc > 5 ? atoi(argv[5]) : 1;
    PIO pio = pio_open(0);
    if (PIO_IS_ERR(pio)) return 1;
    pio_select(pio);
    int smt = pio_claim_unused_sm(pio, true), smc = pio_claim_unused_sm(pio, true);
    uint16_t t_ins[2] = { (uint16_t)(pio_encode_set(pio_pins, 1) | pio_encode_delay(1)),
                          (uint16_t)(pio_encode_set(pio_pins, 0) | pio_encode_delay(1)) };
    pio_program_t tp = { .instructions = t_ins, .length = 2, .origin = -1 };
    uint toff = pio_add_program(pio, &tp);
    uint16_t c_ins[1] = { (uint16_t)(pio_encode_in(pio_pins, 1) | pio_encode_delay((uint)(every - 1))) };
    pio_program_t cp = { .instructions = c_ins, .length = 1, .origin = -1 };
    uint coff = pio_add_program(pio, &cp);

    pio_gpio_init(pio, (uint)gpio);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_set_pins(&c, (uint)gpio, 1);
    sm_config_set_wrap(&c, toff, toff + 1);
    sm_config_set_clkdiv_int_frac(&c, (uint16_t)div, 0);
    pio_sm_init(pio, (uint)smt, toff, &c);
    pio_sm_set_consecutive_pindirs(pio, (uint)smt, (uint)gpio, 1, true);

    c = pio_get_default_sm_config();
    sm_config_set_in_pins(&c, (uint)gpio);
    sm_config_set_wrap(&c, coff, coff);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac(&c, (uint16_t)div, 0);
    pio_sm_init(pio, (uint)smc, coff, &c);
    int r = pio_sm_config_xfer(pio, (uint)smc, PIO_DIR_FROM_SM, (uint)(getenv("CAP_BOUNCE") ? atol(getenv("CAP_BOUNCE")) : bytes), getenv("CAP_COUNT") ? atoi(getenv("CAP_COUNT")) : 1);
    if (r) { fprintf(stderr, "config_xfer: %d\n", r); return 1; }
    pio_enable_sm_in_sync_mask(pio, (1u << smt) | (1u << smc));
    uint8_t *buf = malloc(bytes);
    r = pio_sm_xfer_data(pio, (uint)smc, PIO_DIR_FROM_SM, (uint)bytes, buf);
    pio_set_sm_mask_enabled(pio, (1u << smt) | (1u << smc), false);
    pio_sm_set_consecutive_pindirs(pio, (uint)smt, (uint)gpio, 1, false);
    FILE *o = fopen(argv[4], "wb");
    fwrite(buf, 1, bytes, o);
    fclose(o);
    printf("captest: %zu bytes, xfer %d, PIO clock %.3f MHz, sampling every %d\n",
           bytes, r, 200.0 / div, every);
    pio_close(pio);
    return 0;
}

/*
 * ytest: on each of 4 SMs, load Y from the TX FIFO (with autopull on, as
 * ppplay does), then push Y back through the RX FIFO, and also push the
 * next FIFO word via OSR. Non-blocking accesses only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "piolib.h"

int main(void)
{
    PIO pio = pio_open(0);
    if (PIO_IS_ERR(pio)) return 1;
    pio_select(pio);
    pio_enable_fatal_errors(pio, false);
    /* out y,32 ; mov isr,y ; push block ; out x,32 ; mov isr,x ; push ; halt */
    uint16_t ins[7];
    ins[0] = (uint16_t)pio_encode_out(pio_y, 32);
    ins[1] = (uint16_t)pio_encode_mov(pio_isr, pio_y);
    ins[2] = (uint16_t)pio_encode_push(false, true);
    ins[3] = (uint16_t)pio_encode_out(pio_x, 32);
    ins[4] = (uint16_t)pio_encode_mov(pio_isr, pio_x);
    ins[5] = (uint16_t)pio_encode_push(false, true);
    ins[6] = 0;
    pio_program_t p = { .instructions = ins, .length = 7, .origin = -1 };
    uint off = pio_add_program(pio, &p);
    pio_remove_program(pio, &p, off);
    ins[6] = (uint16_t)pio_encode_jmp(off + 6);
    pio_add_program_at_offset(pio, &p, off);
    for (int k = 0; k < 4; k++) {
        int sm = pio_claim_unused_sm(pio, false);
        if (sm < 0) { printf("no sm\n"); return 1; }
        pio_sm_config c = pio_get_default_sm_config();
        sm_config_set_wrap(&c, off, off + 6);
        sm_config_set_out_shift(&c, true, true, 32);
        sm_config_set_in_shift(&c, true, false, 32);
        sm_config_set_clkdiv_int_frac(&c, 1000, 0);
        pio_sm_init(pio, (uint)sm, off, &c);
        pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_TO_SM, 4096, 2);
        pio_sm_clear_fifos(pio, (uint)sm);
        pio_sm_put(pio, (uint)sm, 30 + (uint)k);
        pio_sm_put(pio, (uint)sm, 0x76543210);
        printf("SM%d: TX level %u before enable", sm, pio_sm_get_tx_fifo_level(pio, (uint)sm));
        pio_sm_set_enabled(pio, (uint)sm, true);
        usleep(20000);
        printf(", TX %u RX %u after:", pio_sm_get_tx_fifo_level(pio, (uint)sm),
               pio_sm_get_rx_fifo_level(pio, (uint)sm));
        while (pio_sm_get_rx_fifo_level(pio, (uint)sm))
            printf(" %08x", pio_sm_get(pio, (uint)sm));
        printf("\n");
        pio_sm_set_enabled(pio, (uint)sm, false);
    }
    pio_close(pio);
    return 0;
}

/*
 * rpi5datv - RP1 PIO output, see pio_out.h
 */
#include "pio_out.h"

#include <stdio.h>
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

    if (pio_sm_config_xfer(g_pio, g_sm, PIO_DIR_TO_SM,
                           (uint)cfg->xfer_bytes, (uint)cfg->xfer_count)) {
        fprintf(stderr, "pio_sm_config_xfer failed (buffers %zu x %d)\n",
                cfg->xfer_bytes, cfg->xfer_count);
        pio_out_close();
        return -1;
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
    return 0;
}

int pio_out_write(const uint32_t *words, size_t nwords)
{
    const uint8_t *p = (const uint8_t *)words;
    size_t bytes = nwords * 4;
    while (bytes) {
        size_t chunk = bytes < g_cfg.xfer_bytes ? bytes : g_cfg.xfer_bytes;
        int ret = pio_sm_xfer_data(g_pio, (uint)g_sm, PIO_DIR_TO_SM,
                                   (uint)chunk, (void *)p);
        if (ret)
            return ret < 0 ? ret : -ret;
        p += chunk;
        bytes -= chunk;
    }
    return 0;
}

void pio_out_close(void)
{
    if (!g_pio)
        return;
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

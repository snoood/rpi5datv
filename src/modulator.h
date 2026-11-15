/*
 * rpi5datv - 1-bit direct digital synthesis QPSK modulator
 *
 * The RP1 PIO on the Pi 5 has no tunable clock: it runs from the fixed
 * 200 MHz clk_sys through a divider whose fractional part is implemented
 * by skipping cycles (5 ns of jitter - fatal for an RF carrier). So the
 * PIO runs with an *integer* divider as a plain 1-bit DAC (one GPIO
 * sample per PIO clock) and the carrier is synthesised in software:
 *
 *   bit[n] = cos(2*pi*f0*n/fs + phase(symbol)) >= 0
 *
 * f0 is chosen so that one of the sampling images (m*fs +/- f0) lands
 * exactly on the requested output frequency. A 32-bit phase accumulator
 * gives ~0.05 Hz tuning resolution. Images of type m*fs - f0 are
 * spectrally inverted, which is compensated by negating the symbol phase.
 */
#ifndef RPI5DATV_MODULATOR_H
#define RPI5DATV_MODULATOR_H

#include <stdint.h>
#include <stddef.h>

#define RP1_CLK_SYS_HZ 200000000.0

typedef struct {
    double target_hz;      /* requested RF frequency */
    int div;               /* PIO integer clock divider */
    double fs;             /* sample rate = 200 MHz / div */
    int image;             /* m: target = m*fs + f0 */
    double f0;             /* signed baseband frequency, |f0| <= fs/2 */
    int inverted;          /* f0 < 0: image is spectrally inverted */
    double gain_db;        /* sample-and-hold rolloff at target */
    double actual_hz;      /* frequency after phase-increment rounding */
    double mer_db;         /* simulated modulation error ratio of the image */
    int spur_warning;      /* MER below 20 dB: aliased harmonics in band */
    uint32_t phase_inc;
} freqplan_t;

/*
 * Choose the PIO divider for a target frequency.
 *  forced_div > 0 forces a divider, otherwise the best one is picked
 *  among those whose stream rate (fs/8 bytes/s) <= max_bytes_per_s.
 * Returns 0 on success, -1 (with message in err) if impossible.
 */
int freqplan_choose(double target_hz, double symbol_rate, int forced_div,
                    double max_bytes_per_s, freqplan_t *out,
                    char *err, size_t errlen);

/* Simulated MER (dB) of a plan; used by freqplan_choose, exposed for tests. */
double freqplan_simulate_mer(const freqplan_t *fp, double symbol_rate, int nsym);

typedef struct {
    uint32_t acc;          /* carrier phase accumulator */
    uint32_t inc;
    int inverted;
    uint64_t t_frac;       /* symbol clock, 32.32 fixed point samples */
    uint64_t t_step;       /* samples per symbol, 32.32 */
    uint32_t cur_word;     /* partially filled output word */
    int bitpos;            /* bits already in cur_word */
} dds_t;

void dds_init(dds_t *d, const freqplan_t *fp, double symbol_rate);

/* Upper bound on 32-bit words produced for nsym symbols. */
size_t dds_max_words(const dds_t *d, size_t nsym);

/*
 * Modulate nsym symbols (values 0..3) into packed 32-bit words, LSB
 * first (the PIO shifts right). Returns number of complete words written.
 */
size_t dds_modulate(dds_t *d, const uint8_t *sym, size_t nsym, uint32_t *out);

#endif

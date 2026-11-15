/*
 * rpi5datv - 1-bit DDS QPSK modulator, see modulator.h
 */
#include "modulator.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <complex.h>
#include <stdlib.h>

#define OCCUPIED_BW_FACTOR 1.35   /* rectangular-ish QPSK main lobe guard */
#define MER_GOOD_DB 20.0          /* plenty for any DVB-S code rate */
#define MER_SIM_SYMBOLS 1500

static double sinc_abs(double x)
{
    if (fabs(x) < 1e-12)
        return 1.0;
    return fabs(sin(M_PI * x) / (M_PI * x));
}

static void evaluate(double target, double sr, int div, freqplan_t *p, int *ok)
{
    memset(p, 0, sizeof(*p));
    p->target_hz = target;
    p->div = div;
    p->fs = RP1_CLK_SYS_HZ / div;
    p->image = (int)lround(target / p->fs);
    p->f0 = target - p->image * p->fs;
    p->inverted = p->f0 < 0;
    p->gain_db = 20.0 * log10(sinc_abs(target / p->fs));

    double af0 = fabs(p->f0);
    double bw = OCCUPIED_BW_FACTOR * sr;

    /* The +f0 and -f0 images (and DC) must not overlap the signal. */
    *ok = (af0 >= bw) && (af0 <= p->fs / 2 - bw);

    double inc = af0 / p->fs * 4294967296.0;
    if (inc >= 4294967295.0)
        inc = 4294967295.0;
    p->phase_inc = (uint32_t)llround(inc);
    double f0_actual = p->phase_inc * p->fs / 4294967296.0;
    p->actual_hz = p->image * p->fs + (p->inverted ? -f0_actual : f0_actual);
}

/*
 * Simulate the 1-bit DDS for a burst of random symbols and measure the
 * modulation error ratio a receiver would see on the wanted image after
 * an (ideal, rectangular) matched filter. This captures the in-band
 * aliases of the carrier's odd harmonics, which is what limits quality.
 */
double freqplan_simulate_mer(const freqplan_t *fp, double sr, int nsym)
{
    if (nsym < 16)
        return 0;
    uint8_t *sym = malloc((size_t)nsym);
    double complex *r = malloc(sizeof(double complex) * (size_t)nsym);
    dds_t d;
    dds_init(&d, fp, sr);
    uint32_t *w = malloc(dds_max_words(&d, (size_t)nsym) * sizeof(uint32_t));
    if (!sym || !r || !w) {
        free(sym); free(r); free(w);
        return 0;
    }
    unsigned seed = 12345;
    for (int i = 0; i < nsym; i++) {
        seed = seed * 1103515245u + 12345u;
        sym[i] = (uint8_t)((seed >> 16) & 3);
    }
    size_t nw = dds_modulate(&d, sym, (size_t)nsym, w);
    const double sps = fp->fs / sr;
    const double complex step = cexp(-I * 2.0 * M_PI * fabs(fp->f0) / fp->fs);
    int n_ok = 0;
    for (int k = 0; k < nsym; k++) {
        long a = (long)ceil(k * sps), b = (long)ceil((k + 1) * sps);
        if ((size_t)b > nw * 32)
            break;
        double complex lo = cexp(-I * 2.0 * M_PI * fabs(fp->f0) * (double)a / fp->fs);
        double complex acc = 0;
        for (long n = a; n < b; n++) {
            int bit = (int)((w[n / 32] >> (n % 32)) & 1u);
            acc += bit ? lo : -lo;
            lo *= step;
        }
        r[k] = fp->inverted ? conj(acc) / (double)(b - a) : acc / (double)(b - a);
        n_ok = k + 1;
    }
    /* strip the modulation, estimate the (constant) carrier rotation */
    const int skip = 4;
    double complex mean = 0;
    for (int k = skip; k < n_ok; k++)
        mean += r[k] * cexp(-I * M_PI / 2 * sym[k]);
    mean /= (n_ok - skip);
    double es = 0, en = 0;
    for (int k = skip; k < n_ok; k++) {
        double complex ref = mean * cexp(I * M_PI / 2 * sym[k]);
        double complex e = r[k] - ref;
        es += creal(ref * conj(ref));
        en += creal(e * conj(e));
    }
    free(sym); free(r); free(w);
    if (en <= 0)
        return 99.0;
    double mer = 10.0 * log10(es / en);
    return mer > 99.0 ? 99.0 : mer;
}

int freqplan_choose(double target_hz, double symbol_rate, int forced_div,
                    double max_bytes_per_s, freqplan_t *out,
                    char *err, size_t errlen)
{
    if (target_hz <= 0) {
        snprintf(err, errlen, "frequency must be positive");
        return -1;
    }

    if (forced_div > 0) {
        int ok;
        evaluate(target_hz, symbol_rate, forced_div, out, &ok);
        if (!ok) {
            snprintf(err, errlen,
                     "with PIO divider %d (fs=%.3f MHz) the carrier sits %.3f MHz "
                     "from a Nyquist edge - the signal would overlap its own image. "
                     "Pick another divider or let rpi5datv choose.",
                     forced_div, out->fs / 1e6, fabs(out->f0) / 1e6);
            return -1;
        }
        out->mer_db = freqplan_simulate_mer(out, symbol_rate, MER_SIM_SYMBOLS);
        out->spur_warning = out->mer_db < MER_GOOD_DB;
        return 0;
    }

    /* Among dividers giving a good MER pick the strongest image;
     * if none is good, take the best MER available. */
    freqplan_t best_good, best_any;
    int have_good = 0, have_any = 0;
    for (int div = 1; div <= 64; div++) {
        double fs = RP1_CLK_SYS_HZ / div;
        if (fs / 8.0 > max_bytes_per_s)
            continue;
        freqplan_t p;
        int ok;
        evaluate(target_hz, symbol_rate, div, &p, &ok);
        if (!ok)
            continue;
        /* quick reject: images weaker than -60 dB are useless */
        if (p.gain_db < -60.0)
            continue;
        p.mer_db = freqplan_simulate_mer(&p, symbol_rate, MER_SIM_SYMBOLS);
        p.spur_warning = p.mer_db < MER_GOOD_DB;
        if (!p.spur_warning) {
            if (!have_good || p.gain_db > best_good.gain_db) {
                best_good = p;
                have_good = 1;
            }
        }
        if (!have_any || p.mer_db > best_any.mer_db) {
            best_any = p;
            have_any = 1;
        }
    }
    if (have_good) {
        *out = best_good;
        return 0;
    }
    if (have_any) {
        *out = best_any;
        return 0;
    }
    snprintf(err, errlen,
             "no PIO divider can place %.3f MHz cleanly within a stream rate of "
             "%.1f MB/s; try --pio-max-rate or a lower symbol rate",
             target_hz / 1e6, max_bytes_per_s / 1e6);
    return -1;
}

void dds_init(dds_t *d, const freqplan_t *fp, double symbol_rate)
{
    memset(d, 0, sizeof(*d));
    d->inc = fp->phase_inc;
    d->inverted = fp->inverted;
    d->t_step = (uint64_t)llround(fp->fs / symbol_rate * 4294967296.0);
}

size_t dds_max_words(const dds_t *d, size_t nsym)
{
    uint64_t samples = ((d->t_step >> 32) + 1) * (uint64_t)nsym;
    return (size_t)(samples / 32 + 2);
}

/* Generate n (1..32) bits starting at phase acc with offset off. */
static inline uint32_t gen_bits(uint32_t acc, uint32_t inc, uint32_t off, int n)
{
    uint32_t w = 0;
    uint32_t ph = acc + off;
    for (int j = 0; j < n; j++) {
        w |= ((~ph) >> 31) << j;
        ph += inc;
    }
    return w;
}

/* Full 32-bit word; written so the compiler can vectorise it. */
static inline uint32_t gen_word(uint32_t acc, uint32_t inc, uint32_t off)
{
    uint32_t w = 0;
    uint32_t base = acc + off;
    for (uint32_t j = 0; j < 32; j++)
        w |= ((~(base + j * inc)) >> 31) << j;
    return w;
}

size_t dds_modulate(dds_t *d, const uint8_t *sym, size_t nsym, uint32_t *out)
{
    size_t nw = 0;
    uint32_t acc = d->acc;
    const uint32_t inc = d->inc;
    uint32_t cur = d->cur_word;
    int bitpos = d->bitpos;

    for (size_t s = 0; s < nsym; s++) {
        d->t_frac += d->t_step;
        uint32_t nsamp = (uint32_t)(d->t_frac >> 32);
        d->t_frac &= 0xFFFFFFFFull;

        uint32_t k = sym[s] & 3u;
        if (d->inverted)
            k = (4u - k) & 3u;
        /* cos(x) >= 0  <=>  (x + 90deg) mod 360 < 180deg */
        uint32_t off = (k << 30) + 0x40000000u;

        /* finish the partially filled word */
        if (bitpos) {
            int n = 32 - bitpos;
            if ((uint32_t)n > nsamp)
                n = (int)nsamp;
            cur |= gen_bits(acc, inc, off, n) << bitpos;
            acc += inc * (uint32_t)n;
            bitpos += n;
            nsamp -= (uint32_t)n;
            if (bitpos == 32) {
                out[nw++] = cur;
                cur = 0;
                bitpos = 0;
            }
        }
        /* whole words */
        while (nsamp >= 32) {
            out[nw++] = gen_word(acc, inc, off);
            acc += inc * 32u;
            nsamp -= 32;
        }
        /* tail */
        if (nsamp) {
            cur = gen_bits(acc, inc, off, (int)nsamp);
            acc += inc * nsamp;
            bitpos = (int)nsamp;
        }
    }
    d->acc = acc;
    d->cur_word = cur;
    d->bitpos = bitpos;
    return nw;
}

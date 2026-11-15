/*
 * rpi5datv - self tests for the DVB-S coder and the DDS modulator.
 *
 * The coder is verified by an independent *decoder* written here:
 * QPSK demap -> depuncture -> Viterbi -> deinterleave -> RS syndrome
 * check -> de-randomise, which must reproduce the input packets exactly.
 * The modulator is verified by demodulating its bit stream.
 *
 * Build/run: make test
 */
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dvbs.h"
#include "../modulator.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* --------------------------------------------------------------- */

static void test_prbs(void)
{
    const uint8_t *p = dvbs_prbs_table();
    /* EN 300 421: PRBS output after init 100101010000000 begins 0x03 0xF6 */
    CHECK(p[0] == 0x03 && p[1] == 0xF6, "PRBS starts %02X %02X, expected 03 F6", p[0], p[1]);
    printf("prbs: %02X %02X %02X %02X ...\n", p[0], p[1], p[2], p[3]);
}

/* c[0] is the highest-degree coefficient. */
static int rs_syndromes_zero(const uint8_t *c)
{
    for (int j = 0; j < 16; j++) {
        uint8_t a = dvbs_gf_exp(j), s = 0;
        for (int i = 0; i < 204; i++)
            s = dvbs_gf_mul(s, a) ^ c[i];
        if (s)
            return 0;
    }
    return 1;
}

static void test_rs(void)
{
    uint8_t in[188], out[204];
    int ok = 1;
    for (int n = 0; n < 200; n++) {
        for (int i = 0; i < 188; i++)
            in[i] = (uint8_t)rand();
        dvbs_rs_encode(in, out);
        if (!rs_syndromes_zero(out) || memcmp(in, out, 188))
            ok = 0;
        out[n % 204] ^= 0x5A;   /* corrupt: syndromes must now be non-zero */
        if (rs_syndromes_zero(out))
            ok = 0;
    }
    CHECK(ok, "RS(204,188) codewords");
    printf("rs: %s\n", ok ? "ok" : "BAD");
}

/* ---------------- reference decoder ------------------------------ */

static const char *px[] = { "1", "10", "101", "10101", "1000101" };
static const char *py[] = { "1", "11", "110", "11010", "1111010" };

/* Viterbi over depunctured soft values (+1/-1, 0 = erasure). */
static void viterbi(const signed char *xs, const signed char *ys, int nbits, uint8_t *out)
{
    enum { NS = 64 };
    int metric[NS], nm[NS];
    uint8_t *dec = malloc((size_t)nbits * NS);
    for (int s = 0; s < NS; s++)
        metric[s] = s ? -1000000 : 0;
    for (int t = 0; t < nbits; t++) {
        for (int s = 0; s < NS; s++)
            nm[s] = -2000000000;
        for (int s = 0; s < NS; s++) {
            if (metric[s] < -900000)
                continue;
            for (int b = 0; b < 2; b++) {
                unsigned reg = (unsigned)b << 6 | (unsigned)s;
                int x = __builtin_parity(reg & 0171), y = __builtin_parity(reg & 0133);
                int m = metric[s] + xs[t] * (x ? -1 : 1) + ys[t] * (y ? -1 : 1);
                int ns = (int)(reg >> 1);
                if (m > nm[ns]) {
                    nm[ns] = m;
                    dec[(size_t)t * NS + ns] = (uint8_t)(s | (b << 7));
                }
            }
        }
        memcpy(metric, nm, sizeof(metric));
    }
    int best = 0;
    for (int s = 1; s < NS; s++)
        if (metric[s] > metric[best])
            best = s;
    memset(out, 0, (size_t)(nbits + 7) / 8);
    for (int t = nbits - 1; t >= 0; t--) {
        uint8_t d = dec[(size_t)t * NS + best];
        int bit = d >> 7;
        if (bit)
            out[t / 8] |= (uint8_t)(0x80 >> (t % 8));
        best = d & 63;
    }
    free(dec);
}

static int run_roundtrip(dvbs_fec_t fec)
{
    const int npk = 48;
    uint8_t *ts = malloc((size_t)npk * 188);
    for (int p = 0; p < npk; p++) {
        ts[p * 188] = 0x47;
        for (int i = 1; i < 188; i++)
            ts[p * 188 + i] = (uint8_t)rand();
    }
    dvbs_enc_t e;
    dvbs_init(&e, fec);
    uint8_t *syms = malloc((size_t)npk * 204 * 8);
    int nsym = 0;
    for (int p = 0; p < npk; p++)
        nsym += dvbs_encode_packet(&e, ts + p * 188, syms + nsym);

    /* QPSK demap to serial bit stream */
    int nser = nsym * 2;
    uint8_t *ser = malloc((size_t)nser);
    for (int i = 0; i < nsym; i++) {
        static const int bi[4] = { 0, 1, 1, 0 }, bq[4] = { 0, 0, 1, 1 };
        ser[2 * i] = (uint8_t)bi[syms[i]];
        ser[2 * i + 1] = (uint8_t)bq[syms[i]];
    }

    /* depuncture */
    int nbits = npk * 204 * 8;
    signed char *xs = calloc((size_t)nbits, 1), *ys = calloc((size_t)nbits, 1);
    int period = (int)strlen(px[fec]);
    int k = 0;
    for (int t = 0; t < nbits && k < nser; t++) {
        int pp = t % period;
        if (px[fec][pp] == '1' && k < nser)
            xs[t] = ser[k++] ? -1 : 1;
        if (py[fec][pp] == '1' && k < nser)
            ys[t] = ser[k++] ? -1 : 1;
    }
    uint8_t *bytes = malloc((size_t)nbits / 8);
    viterbi(xs, ys, nbits, bytes);

    /* deinterleave: branch j delay (11-j)*17 */
    int nbytes = nbits / 8;
    uint8_t *dl = calloc((size_t)nbytes, 1);
    /* interleaver delays branch j by j*17*12 byte slots; the
     * deinterleaver adds (11-j)*17*12 so every byte is delayed 11*204 */
    for (int i = 0; i < nbytes; i++) {
        int j = i % 12;
        int o = i + (11 - j) * 17 * 12;     /* deinterleaver output index */
        if (o < nbytes)
            dl[o] = bytes[i];
    }

    int bad = 0, checked = 0;
    const uint8_t *prbs = dvbs_prbs_table();
    for (int p = 11; p < npk; p++) {
        const uint8_t *c = dl + (size_t)p * 204;
        int q = p - 11;                      /* input packet index */
        if (!rs_syndromes_zero(c)) { bad++; continue; }
        uint8_t expect_sync = (q % 8 == 0) ? 0xB8 : 0x47;
        if (c[0] != expect_sync) { bad++; continue; }
        for (int i = 1; i < 188; i++)
            if ((uint8_t)(c[i] ^ prbs[(q % 8) * 188 + i - 1]) != ts[q * 188 + i]) { bad++; break; }
        checked++;
    }
    /* the last few packets are corrupted by Viterbi tail truncation */
    int ok = checked >= npk - 11 - 2 && bad <= 1;
    printf("roundtrip FEC %s: %d symbols, %d packets verified, %d bad\n",
           dvbs_fec_name(fec), nsym, checked, bad);
    free(ts); free(syms); free(ser); free(xs); free(ys); free(bytes); free(dl);
    return ok;
}

static void test_roundtrip(void)
{
    for (int f = FEC_1_2; f <= FEC_7_8; f++)
        CHECK(run_roundtrip((dvbs_fec_t)f), "roundtrip FEC %s", dvbs_fec_name((dvbs_fec_t)f));
}

/* ---------------- modulator -------------------------------------- */

static void test_dds(double target, double sr, double max_rate)
{
    freqplan_t fp;
    char err[256];
    if (freqplan_choose(target, sr, 0, max_rate, &fp, err, sizeof(err)) < 0) {
        CHECK(0, "plan %.3f MHz: %s", target / 1e6, err);
        return;
    }
    printf("plan %8.3f MHz SR %4.0fk: div %d fs %.2f image %d f0 %+.3f MHz gain %.1f dB%s, err %.3f Hz\n",
           target / 1e6, sr / 1e3, fp.div, fp.fs / 1e6, fp.image, fp.f0 / 1e6, fp.gain_db,
           fp.spur_warning ? " LOW-MER" : "", fp.actual_hz - target);
    CHECK(fabs(fp.actual_hz - target) < 1.0, "frequency error");

    /* independent check with a longer burst than the planner uses */
    double mer = freqplan_simulate_mer(&fp, sr, 6000);
    printf("      MER %.1f dB (planner estimate %.1f dB)\n", mer, fp.mer_db);
    /* DVB-S needs ~4.5 dB (FEC 1/2) to ~8 dB (FEC 7/8) Es/N0 */
    CHECK(mer >= 12.0, "MER too low at %.3f MHz SR %.0fk: %.1f dB", target / 1e6, sr / 1e3, mer);
}

int main(void)
{
    srand(1234);
    test_prbs();
    test_rs();
    test_roundtrip();
    test_dds(437.0e6, 333e3, 9e6);
    test_dds(146.5e6, 333e3, 9e6);
    test_dds(71.0e6, 250e3, 9e6);
    test_dds(1255.0e6, 1000e3, 9e6);
    test_dds(437.0e6, 4000e3, 12.5e6);   /* needs PIO divider 2 */
    test_dds(51.0e6, 125e3, 9e6);
    test_dds(2395.0e6, 2000e3, 9e6);

    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}

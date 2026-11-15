/*
 * rpi5datv - DVB-S channel coding (EN 300 421)
 */
#include "dvbs.h"

#include <string.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* FEC descriptions                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int num, den;
    int period;          /* input bits per puncture period */
    const char *x, *y;   /* puncture masks, '1' = transmit */
} fec_desc_t;

static const fec_desc_t fec_table[] = {
    [FEC_1_2] = { "1/2", 1, 2, 1, "1",       "1"       },
    [FEC_2_3] = { "2/3", 2, 3, 2, "10",      "11"      },
    [FEC_3_4] = { "3/4", 3, 4, 3, "101",     "110"     },
    [FEC_5_6] = { "5/6", 5, 6, 5, "10101",   "11010"   },
    [FEC_7_8] = { "7/8", 7, 8, 7, "1000101", "1111010" },
};

int dvbs_parse_fec(const char *s, dvbs_fec_t *out)
{
    for (int i = 0; i < (int)(sizeof(fec_table) / sizeof(fec_table[0])); i++) {
        if (strcmp(s, fec_table[i].name) == 0) {
            *out = (dvbs_fec_t)i;
            return 0;
        }
    }
    return -1;
}

const char *dvbs_fec_name(dvbs_fec_t fec) { return fec_table[fec].name; }

void dvbs_fec_ratio(dvbs_fec_t fec, int *num, int *den)
{
    *num = fec_table[fec].num;
    *den = fec_table[fec].den;
}

double dvbs_ts_bitrate(double symbol_rate, dvbs_fec_t fec)
{
    /* 2 bits/symbol * code rate * RS 188/204 */
    return symbol_rate * 2.0 * fec_table[fec].num / fec_table[fec].den
           * 188.0 / 204.0;
}

/* ------------------------------------------------------------------ */
/* GF(256), primitive polynomial x^8+x^4+x^3+x^2+1                     */
/* ------------------------------------------------------------------ */

static uint8_t gf_exp_tab[512];
static uint8_t gf_log_tab[256];
static uint8_t rs_gen[17];          /* generator poly, rs_gen[16] = 1 */
static uint8_t prbs_tab[1503];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void build_tables(void)
{
    /* GF tables */
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp_tab[i] = (uint8_t)x;
        gf_log_tab[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++)
        gf_exp_tab[i] = gf_exp_tab[i - 255];

    /* g(x) = prod_{i=0}^{15} (x + a^i), coefficients low order first */
    memset(rs_gen, 0, sizeof(rs_gen));
    rs_gen[0] = 1;
    for (int i = 0; i < 16; i++) {
        uint8_t root = gf_exp_tab[i];
        /* multiply current poly (degree i) by (x + root) */
        for (int k = i + 1; k > 0; k--)
            rs_gen[k] = rs_gen[k - 1] ^ dvbs_gf_mul(rs_gen[k], root);
        rs_gen[0] = dvbs_gf_mul(rs_gen[0], root);
    }

    /* Energy dispersal PRBS 1 + x^14 + x^15, init 100101010000000 */
    int s[16] = { 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 };
    for (int n = 0; n < 1503; n++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            int out = s[14] ^ s[15];
            for (int k = 15; k > 1; k--)
                s[k] = s[k - 1];
            s[1] = out;
            byte = (uint8_t)((byte << 1) | out);
        }
        prbs_tab[n] = byte;
    }
}

static void ensure_tables(void) { pthread_once(&tables_once, build_tables); }

uint8_t dvbs_gf_mul(uint8_t a, uint8_t b)
{
    if (!a || !b)
        return 0;
    return gf_exp_tab[gf_log_tab[a] + gf_log_tab[b]];
}

uint8_t dvbs_gf_exp(int i)
{
    ensure_tables();
    i %= 255;
    if (i < 0)
        i += 255;
    return gf_exp_tab[i];
}

const uint8_t *dvbs_prbs_table(void)
{
    ensure_tables();
    return prbs_tab;
}

/* ------------------------------------------------------------------ */
/* Reed-Solomon RS(204,188), shortened from RS(255,239)               */
/* ------------------------------------------------------------------ */

void dvbs_rs_encode(const uint8_t *in188, uint8_t *out204)
{
    ensure_tables();
    uint8_t r[16] = { 0 };          /* r[15] = highest degree */

    for (int i = 0; i < 188; i++) {
        uint8_t fb = in188[i] ^ r[15];
        if (fb) {
            uint8_t lfb = gf_log_tab[fb];
            for (int k = 15; k > 0; k--)
                r[k] = r[k - 1] ^ (rs_gen[k] ? gf_exp_tab[lfb + gf_log_tab[rs_gen[k]]] : 0);
            r[0] = rs_gen[0] ? gf_exp_tab[lfb + gf_log_tab[rs_gen[0]]] : 0;
        } else {
            for (int k = 15; k > 0; k--)
                r[k] = r[k - 1];
            r[0] = 0;
        }
    }
    if (out204 != in188)
        memcpy(out204, in188, 188);
    for (int k = 0; k < 16; k++)
        out204[188 + k] = r[15 - k];
}

/* ------------------------------------------------------------------ */
/* Encoder                                                            */
/* ------------------------------------------------------------------ */

void dvbs_init(dvbs_enc_t *e, dvbs_fec_t fec)
{
    ensure_tables();
    memset(e, 0, sizeof(*e));
    e->fec = fec;
    e->pending_bit = -1;
}

static inline int parity7(unsigned v)
{
    return __builtin_parity(v);
}

int dvbs_encode_packet(dvbs_enc_t *e, const uint8_t *ts188, uint8_t *sym_out)
{
    uint8_t pkt[RS_PACKET_SIZE];
    const fec_desc_t *fd = &fec_table[e->fec];

    /* 1. energy dispersal: group of 8 packets, first sync inverted */
    const int p = e->packet_in_group;
    pkt[0] = (p == 0) ? 0xB8 : 0x47;
    const uint8_t *prbs = prbs_tab + p * 188;
    for (int i = 1; i < 188; i++)
        pkt[i] = ts188[i] ^ prbs[i - 1];
    e->packet_in_group = (p + 1) & 7;

    /* 2. RS(204,188) */
    dvbs_rs_encode(pkt, pkt);

    /* 3. convolutional interleaver I=12, M=17. Sync byte goes to branch 0. */
    for (int i = 0; i < RS_PACKET_SIZE; i++) {
        int j = e->il_branch;
        if (j) {
            int len = j * 17;
            uint8_t out = e->il_mem[j][e->il_pos[j]];
            e->il_mem[j][e->il_pos[j]] = pkt[i];
            if (++e->il_pos[j] == len)
                e->il_pos[j] = 0;
            pkt[i] = out;
        }
        if (++e->il_branch == 12)
            e->il_branch = 0;
    }

    /* 4. convolutional code + puncturing + QPSK mapping */
    int nsym = 0;
    unsigned state = e->conv_state;
    int pp = e->punct_pos;
    int pend = e->pending_bit;
    for (int i = 0; i < RS_PACKET_SIZE; i++) {
        uint8_t byte = pkt[i];
        for (int b = 7; b >= 0; b--) {
            unsigned reg = (((unsigned)byte >> b) & 1u) << 6 | state;
            int x = parity7(reg & 0171);
            int y = parity7(reg & 0133);
            state = reg >> 1;

            int outbits[2];
            int nout = 0;
            if (fd->x[pp] == '1')
                outbits[nout++] = x;
            if (fd->y[pp] == '1')
                outbits[nout++] = y;
            if (++pp == fd->period)
                pp = 0;

            for (int k = 0; k < nout; k++) {
                if (pend < 0) {
                    pend = outbits[k];
                } else {
                    sym_out[nsym++] = dvbs_bits_to_sym(pend, outbits[k]);
                    pend = -1;
                }
            }
        }
    }
    e->conv_state = state;
    e->punct_pos = pp;
    e->pending_bit = pend;
    return nsym;
}

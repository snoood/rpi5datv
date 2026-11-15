/*
 * rpi5datv - DVB-S channel coding (EN 300 421)
 *
 * Transport stream packets (188 bytes) in, QPSK symbols out.
 *   energy dispersal -> RS(204,188) -> Forney interleaver (I=12)
 *   -> K=7 convolutional code (171/133 octal) -> puncturing -> QPSK map
 *
 * Portable C, no assembly: replaces dvbsenco8.s / fec100.c of the
 * original 32-bit rpidatv.
 */
#ifndef RPI5DATV_DVBS_H
#define RPI5DATV_DVBS_H

#include <stdint.h>
#include <stddef.h>

#define TS_PACKET_SIZE 188
#define RS_PACKET_SIZE 204

typedef enum {
    FEC_1_2 = 0,
    FEC_2_3,
    FEC_3_4,
    FEC_5_6,
    FEC_7_8,
} dvbs_fec_t;

/* Parse "1/2", "2/3", "3/4", "5/6", "7/8". Returns -1 on error. */
int dvbs_parse_fec(const char *s, dvbs_fec_t *out);
const char *dvbs_fec_name(dvbs_fec_t fec);
/* Code rate numerator/denominator (e.g. 3,4). */
void dvbs_fec_ratio(dvbs_fec_t fec, int *num, int *den);

/* Useful (TS) bit rate for a given symbol rate in symbols/s. */
double dvbs_ts_bitrate(double symbol_rate, dvbs_fec_t fec);

typedef struct {
    dvbs_fec_t fec;
    /* energy dispersal */
    int packet_in_group;          /* 0..7 */
    /* interleaver: 12 branches, branch j holds j*17 bytes */
    uint8_t il_mem[12][11 * 17];
    int il_pos[12];
    int il_branch;
    /* convolutional encoder */
    unsigned conv_state;          /* last 6 input bits, d1 at bit 5 */
    int punct_pos;                /* position inside puncture period */
    int pending_bit;              /* -1 or I bit waiting for its Q partner */
} dvbs_enc_t;

void dvbs_init(dvbs_enc_t *e, dvbs_fec_t fec);

/*
 * Encode one 188-byte TS packet (must start with 0x47).
 * Writes QPSK symbols (values 0..3, see below) into sym_out, returns the
 * number written. sym_out must hold at least 204*8 = 1632 entries.
 *
 * Symbol value k means carrier phase 45deg + k*90deg, i.e.
 *   k=0: (I,Q) bits 00  -> (+,+)
 *   k=1: (I,Q) bits 10  -> (-,+)
 *   k=2: (I,Q) bits 11  -> (-,-)
 *   k=3: (I,Q) bits 01  -> (+,-)
 */
int dvbs_encode_packet(dvbs_enc_t *e, const uint8_t *ts188, uint8_t *sym_out);

/* Exposed for tests. */
void dvbs_rs_encode(const uint8_t *in188, uint8_t *out204);
uint8_t dvbs_gf_mul(uint8_t a, uint8_t b);
uint8_t dvbs_gf_exp(int i);
const uint8_t *dvbs_prbs_table(void);   /* 1503 bytes */

/* Map (I bit, Q bit) to symbol value and back. */
static inline uint8_t dvbs_bits_to_sym(int bi, int bq)
{
    static const uint8_t map[4] = { 0, 3, 1, 2 }; /* index = bi*2+bq */
    return map[(bi << 1) | bq];
}

#endif

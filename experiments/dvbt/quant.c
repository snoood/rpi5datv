/*
 * quant: real float32 samples -> N-bit DAC codes, with optional
 * band-pass noise shaping that notches quantisation noise out of the
 * channel at the IF (the noise goes to frequencies that end up in other
 * images, outside the TV channel).
 *
 *   quant BITS ORDER IF/FS CLIP in.f32 out.u8
 *
 * ORDER 0 = plain rounding, 2 = NTF (1 - 2cos(w) z^-1 + z^-2),
 * 4 = that squared. CLIP = input level (in RMS units) mapped to full
 * scale. Output: one byte per sample, code 0 .. 2^BITS-1.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: quant BITS ORDER IF/FS CLIP in.f32 out.u8\n");
        return 2;
    }
    int bits = atoi(argv[1]), order = atoi(argv[2]);
    double w = 2 * M_PI * atof(argv[3]), clip = atof(argv[4]);
    FILE *in = fopen(argv[5], "rb"), *out = fopen(argv[6], "wb");
    if (!in || !out) { perror("open"); return 1; }

    /* NTF coefficients: e-feedback filter h[1..order] */
    double h[5] = { 1, 0, 0, 0, 0 };
    double c1 = -2 * cos(w);
    if (order == 2) { h[1] = c1; h[2] = 1; }
    if (order == 4) { h[1] = 2 * c1; h[2] = 2 + c1 * c1; h[3] = 2 * c1; h[4] = 1; }

    /* RMS of the input, for scaling */
    double ss = 0; long n = 0; float f;
    while (fread(&f, 4, 1, in) == 1) { ss += (double)f * f; n++; }
    double rms = sqrt(ss / n);
    rewind(in);

    const int levels = 1 << bits;
    const double half = (levels - 1) / 2.0;
    const double scale = half / (clip * rms);
    double e[5] = { 0 };
    long clipped = 0;
    static float buf[65536];
    static unsigned char ob[65536];
    size_t got;
    while ((got = fread(buf, 4, 65536, in)) > 0) {
        for (size_t i = 0; i < got; i++) {
            double x = buf[i] * scale;              /* -half .. +half nominal */
            double v = x;
            for (int k = 1; k <= order; k++) v += h[k] * e[k];
            /* output y = x + NTF*e, e = y - v */
            double q = floor(v + half + 0.5);
            if (q < 0) { q = 0; clipped++; }
            if (q > levels - 1) { q = levels - 1; clipped++; }
            double err = (q - half) - v;
            if (err > 1.5) err = 1.5;               /* keep the loop stable when clipping */
            if (err < -1.5) err = -1.5;
            for (int k = order; k > 1; k--) e[k] = e[k - 1];
            e[1] = err;
            ob[i] = (unsigned char)q;
        }
        fwrite(ob, 1, got, out);
    }
    fprintf(stderr, "quant: %ld samples, %d bits, order %d, clipped %.4f%%\n",
            n, bits, order, 100.0 * clipped / n);
    fclose(out);
    return 0;
}

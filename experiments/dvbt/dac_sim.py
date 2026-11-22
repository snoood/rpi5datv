#!/usr/bin/env python3
"""Multi-bit GPIO DAC model for DVB-T through a sampling image.

  dac_sim.py prep  BW in.cf32 out.f32              DVB-T IQ -> real IF samples at FS
  dac_sim.py pack  BW codes.u8 BITS PULL out.u32     codes -> PIO words (LSB first)
  dac_sim.py gaps  BW codes.u8 out.u8 BUF GAP JITTER NBUF
        what the PIO actually plays: after every BUF samples it stalls,
        holding the last code, for GAP (+/- uniform JITTER) samples
  dac_sim.py image BW codes.u8 BITS out.cf32 [gain_sigma skew_ps seed]
        DAC codes -> what a tuner sees at the RF image (complex baseband
        at the DVB-T elementary rate), with per-bit gain error (fraction,
        RMS) and per-bit timing skew (ps, RMS).

The PIO runs at FS = 200 MHz / DIV and puts the channel at IF = FS / 4.
The tuner listens on the image RF = K * FS + IF (not inverted).
"""
import os
import sys
from fractions import Fraction
import numpy as np
from scipy.signal import resample_poly
import dvbt_common as c

# BW (MHz) -> PIO divider (override with DIV=n); target RF with RF=MHz
DIV = {8: 6, 7: 7, 6: 8}


def plan(bw):
    """-> fs, signed IF, exact RF. The channel is placed at +IF (upright
    image at k*fs + IF) or at -IF (upright image at k*fs - IF), with
    |IF| kept between 0.12 and 0.38 of fs, clear of DC and fs/2."""
    div = int(os.environ.get('DIV', DIV[bw]))
    fs = 200e6 / div
    rf = float(os.environ.get('RF', 474)) * 1e6
    best = None
    for k in range(1, int(rf / fs) + 2):
        for sign in (1, -1):
            fif = sign * (rf - k * fs)
            if 0.12 * fs <= fif <= 0.38 * fs:
                cand = (abs(fif / fs - 0.25), fs, sign * fif, rf)
                best = min(best, cand) if best else cand
    if not best:
        sys.exit(f'{rf / 1e6} MHz cannot be reached with fs {fs / 1e6:.3f} MS/s')
    return best[1:]


def ratio(bw):
    fs, _, _ = plan(bw)
    r = Fraction(fs / c.elem_rate(bw)).limit_denominator(1000)
    return r.numerator, r.denominator


def prep(bw, src, dst):
    up, down = ratio(bw)
    fs, fif, rf = plan(bw)
    iq = np.fromfile(src, np.complex64)
    x = resample_poly(iq, up, down)
    n = np.arange(len(x))
    lo = np.exp(2j * np.pi * (fif / fs) * n)
    y = (x * lo).real.astype(np.float32)
    y.tofile(dst)
    print(f'{bw} MHz: FS {fs / 1e6:.3f} MS/s, IF {fif / 1e6:+.3f} MHz (notch {abs(fif) / fs:.4f}), '
          f'image at {rf / 1e6:.3f} MHz, {len(y)} samples, '
          f'PAPR {20 * np.log10(np.max(np.abs(y)) / np.sqrt(np.mean(y ** 2))):.1f} dB')


def image(bw, codes, bits, dst, gsig=0.0, skew_ps=0.0, seed=1):
    bits, gsig, skew_ps, seed = int(bits), float(gsig), float(skew_ps), int(seed)
    up, down = ratio(bw)
    fs, fif, rf = plan(bw)
    q = np.fromfile(codes, np.uint8)
    n = np.arange(len(q))
    lo = np.exp(-2j * np.pi * (fif / fs) * n)
    rng = np.random.default_rng(seed)
    g = 1 + gsig * rng.standard_normal(bits)
    tau = skew_ps * 1e-12 * rng.standard_normal(bits)
    out = 0
    for i in range(bits):
        b = ((q >> i) & 1).astype(np.float32) - 0.5
        bb = resample_poly(b * lo, down, up)      # channel filter + to elementary rate
        out = out + (2 ** i) * g[i] * np.exp(-2j * np.pi * rf * tau[i]) * bb
    out = out / np.sqrt(np.mean(np.abs(out) ** 2))
    out.astype(np.complex64).tofile(dst)
    # ideal (error-free) in-band SNR of the codes vs nothing: reported by the rx MER
    print(f'image: {bits} bits, gain sigma {gsig * 100:.1f}%, skew {skew_ps:.0f} ps '
          f'(= {360 * rf * skew_ps * 1e-12:.1f} deg at {rf / 1e6:.1f} MHz), '
          f'gains {np.round(g, 3).tolist()}, skews ps {np.round(tau * 1e12).tolist()}')


def pack(bw, codes, bits, pull, dst):
    bits, pull = int(bits), int(pull)
    spw = pull // bits
    q = np.fromfile(codes, np.uint8).astype(np.uint32)
    q = q[:len(q) // spw * spw].reshape(-1, spw)
    words = np.zeros(len(q), np.uint32)
    for k in range(spw):
        words |= q[:, k] << np.uint32(k * bits)
    words.astype('<u4').tofile(dst)
    print(f'pack: {len(words)} words, {spw} samples/word')


def gaps(bw, codes, dst, buf, gap, jitter, nbuf):
    buf, gap, jitter, nbuf = int(buf), int(gap), int(jitter), int(nbuf)
    q = np.memmap(codes, np.uint8, 'r')
    rng = np.random.default_rng(7)
    with open(dst, 'wb') as f:
        for i in range(min(nbuf, len(q) // buf)):
            seg = np.asarray(q[i * buf:(i + 1) * buf])
            seg.tofile(f)
            g = gap + (rng.integers(-jitter, jitter + 1) if jitter else 0)
            np.full(g, seg[-1], np.uint8).tofile(f)
    print(f'gaps: {nbuf} buffers of {buf}, stall {gap} +/- {jitter} samples after each')


if __name__ == '__main__':
    cmd, bw = sys.argv[1], int(sys.argv[2])
    {'prep': prep, 'image': image, 'pack': pack, 'gaps': gaps}[cmd](bw, *sys.argv[3:])

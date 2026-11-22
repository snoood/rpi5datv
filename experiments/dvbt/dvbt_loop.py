#!/usr/bin/env python3
"""Make a seamlessly loopable DVB-T DAC sample file from a TS.

    RF=474 dvbt_loop.py BW in.ts out.f32
    RF=474 dvbt_loop.py frame BW in.f32 out.f32 SYMS GAP

frame: hide the PIO's DMA gaps. The rp1-pio driver plays its bounce
buffers back to back but the PIO stalls for a fixed ~16 us (GAP samples)
between buffers. Cut the signal into buffers of exactly SYMS OFDM
symbols, each starting inside a guard interval, and delete the first GAP
samples of every buffer: the stall then stands in for the deleted
samples, every symbol's useful part stays on time and the receiver only
sees a slightly shorter guard interval once per buffer. Play the result
with XFER_BYTES = one buffer (printed).

The TS is cut to a whole number of 504-packet periods (lcm of the
8-packet energy-dispersal group and the 252 RS packets per 2k QPSK 1/2
superframe), so one pass of the TS is exactly 2*m superframes. The
modulator runs on the TS looped, and the output of the SECOND pass
(interleavers and filters warmed up, identical to every later pass) is
written: real IF samples at the PIO rate, ready for `quant`.
"""
import os
import sys
import numpy as np
from fractions import Fraction
from gnuradio import gr, blocks, digital, dtv, filter
from gnuradio.filter import firdes
from gnuradio.fft import window
import dvbt_common as c
from dac_sim import plan, ratio

PERIOD_PK = 504                       # TS packets per loop unit
SF_SAMPLES = 4 * 68 * (c.FFT + c.CP)  # samples per superframe (elementary rate)


def make_taps(bw):
    up, _ = ratio(bw)
    fe = c.elem_rate(bw)
    return firdes.low_pass(up, up * fe, 4.1e6 * bw / 8, 0.5e6 * bw / 8, window.WIN_BLACKMAN_HARRIS)


def frame(bw, src, dst, syms, gap):
    bw, syms, gap = int(bw), int(syms), int(gap)
    fs, fif, rf = plan(bw)
    up, down = ratio(bw)
    sym = (c.FFT + c.CP) * up / down                 # PIO samples per OFDM symbol
    buf = syms * sym
    assert buf == int(buf), f'{syms} symbols is not a whole number of samples'
    buf = int(buf)
    x = np.memmap(src, np.float32, 'r')
    period = len(x)
    if period % buf:
        sys.exit(f'{syms} symbols does not divide the loop ({period / sym:.0f} symbols)')
    # Symbol boundaries in x are delayed by the resampler's group delay.
    # Start each buffer a little into a guard interval, so that the
    # deleted samples (and the jitter either side) stay inside it.
    delay = (len(make_taps(bw)) - 1) / 2 / down
    o = int(round(delay)) + 150
    cp = c.CP * up / down
    assert o + gap + 150 < cp, 'gap too long for the guard interval'
    n = period // buf
    with open(dst, 'wb') as f:
        for i in range(n):
            a = (o + i * buf + gap) % period
            b = (o + (i + 1) * buf) % period
            seg = x[a:b] if a < b else np.concatenate([x[a:], x[:b]])
            np.asarray(seg, np.float32).tofile(f)
    each = buf - gap
    print(f'frame: {n} buffers of {syms} symbols = {buf} samples, {gap} deleted at each '
          f'(guard interval {cp:.0f} samples, buffers start {o} into it); '
          f'{each} samples per buffer: XFER_BYTES={each // 2} for 4 bits, '
          f'{each * 4 // 10 if each % 10 == 0 else "n/a"} for 3 bits')


def main(bw, ts, out):
    bw = int(bw)
    fs, fif, rf = plan(bw)
    up, down = ratio(bw)
    pk = os.path.getsize(ts) // 188
    q = Fraction(fif / fs).limit_denominator(100000).denominator   # LO period
    m0 = next(k for k in range(1, 1000)
              if (k * 2 * SF_SAMPLES * up) % down == 0
              and (k * 2 * SF_SAMPLES * up // down) % q == 0)
    m = pk // PERIOD_PK // m0 * m0
    if m < 1:
        sys.exit(f'TS too short: need at least {m0 * PERIOD_PK} packets')
    cut = f'{out}.cut.ts'
    with open(ts, 'rb') as f, open(cut, 'wb') as g:
        g.write(f.read(m * PERIOD_PK * 188))
    period_e = m * 2 * SF_SAMPLES                    # elementary-rate samples
    assert period_e * up % down == 0
    period = period_e * up // down                   # PIO-rate samples
    print(f'{bw} MHz @ {rf / 1e6:.3f} MHz: loop {m * PERIOD_PK} packets = {period / fs:.3f} s '
          f'= {period} samples at {fs / 1e6:.3f} MS/s, IF {fif / 1e6:+.3f} MHz')

    taps = make_taps(bw)
    # exact LO: IF/fs is rational, so the LO repeats every q samples
    r = Fraction(fif / fs).limit_denominator(100000)
    assert abs(float(r) - fif / fs) < 1e-12 and period % r.denominator == 0
    lut = np.exp(2j * np.pi * float(r) * np.arange(r.denominator)).astype(np.complex64)
    lo = blocks.vector_source_c(lut.tolist(), True)
    mix = blocks.multiply_cc()
    tb = gr.top_block()
    chain = [
        blocks.file_source(gr.sizeof_char, cut, True),
        dtv.dvbt_energy_dispersal(1),
        dtv.dvbt_reed_solomon_enc(2, 8, 0x11d, 255, 239, 8, 51, 8),
        dtv.dvbt_convolutional_interleaver(136, 12, 17),
        dtv.dvbt_inner_coder(1, c.DATA, c.CONST, dtv.NH, c.RATE),
        dtv.dvbt_bit_inner_interleaver(c.DATA, c.CONST, dtv.NH, c.MODE),
        dtv.dvbt_symbol_inner_interleaver(c.DATA, c.MODE, 1),
        dtv.dvbt_map(c.DATA, c.CONST, dtv.NH, c.MODE, 1),
        dtv.dvbt_reference_signals(gr.sizeof_gr_complex, c.DATA, c.FFT, c.CONST,
                                   dtv.NH, c.RATE, c.RATE, c.GI, c.MODE, 0, 0),
        digital.ofdm_cyclic_prefixer(c.FFT, c.FFT + c.CP, 0, ''),
        filter.rational_resampler_ccc(up, down, taps),
        mix,
        blocks.complex_to_real(),
        blocks.skiphead(gr.sizeof_float, period),
        blocks.head(gr.sizeof_float, period),
        blocks.file_sink(gr.sizeof_float, out, False),
    ]
    for a, b in zip(chain, chain[1:]):
        tb.connect(a, b)
    tb.connect(lo, (mix, 1))
    tb.run()
    os.remove(cut)
    print(f'wrote {out}: {os.path.getsize(out) // 4} samples')


if __name__ == '__main__':
    if sys.argv[1] == 'frame':
        frame(*sys.argv[2:7])
    else:
        main(*sys.argv[1:4])

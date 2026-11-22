#!/usr/bin/env python3
"""DVB-T complex baseband (cf32 at the elementary rate) -> TS, plus MER.

    dvbt_rx.py in.cf32 out.ts [reference.ts]

Prints the MER of the equalised data carriers and, given the TS that
was transmitted, how many 188-byte packets came back intact.
"""
import sys
import numpy as np
from gnuradio import gr, blocks, dtv, fft
from gnuradio.fft import window
import dvbt_common as c


def main(iq, out, ref=None):
    tb = gr.top_block()
    src = blocks.file_source(gr.sizeof_gr_complex, iq, False)
    acq = dtv.dvbt_ofdm_sym_acquisition(1, c.FFT, c.CARRIERS, c.CP, 30)
    dft = fft.fft_vcc(c.FFT, True, window.rectangular(c.FFT), True, 1)
    ref_sig = dtv.dvbt_demod_reference_signals(gr.sizeof_gr_complex, c.FFT, c.DATA, c.CONST,
                                               dtv.NH, c.RATE, c.RATE, c.GI, c.MODE, 0, 0)
    carriers = '/tmp/dvbt/_carriers.cf32'
    csink = blocks.file_sink(gr.sizeof_gr_complex * c.DATA, carriers, False)
    chain = [
        src, acq, dft, ref_sig,
        dtv.dvbt_demap(c.DATA, c.CONST, dtv.NH, c.MODE, 1),
        dtv.dvbt_symbol_inner_interleaver(c.DATA, c.MODE, 0),
        dtv.dvbt_bit_inner_deinterleaver(c.DATA, c.CONST, dtv.NH, c.MODE),
        blocks.vector_to_stream(gr.sizeof_char, c.DATA),
        dtv.dvbt_viterbi_decoder(c.CONST, dtv.NH, c.RATE, 768),
        dtv.dvbt_convolutional_deinterleaver(136, 12, 17),
        dtv.dvbt_reed_solomon_dec(2, 8, 0x11d, 255, 239, 8, 51, 8),
        dtv.dvbt_energy_descramble(8),
        blocks.file_sink(gr.sizeof_char, out, False),
    ]
    for a, b in zip(chain, chain[1:]):
        tb.connect(a, b)
    tb.connect(ref_sig, csink)
    tb.run()

    z = np.fromfile(carriers, np.complex64)
    z = z[len(z) // 4:]                      # skip acquisition / settling
    if len(z):
        z = z / np.sqrt(np.mean(np.abs(z) ** 2))
        ideal = (np.sign(z.real) + 1j * np.sign(z.imag)) / np.sqrt(2)
        mer = 10 * np.log10(1 / np.mean(np.abs(z - ideal) ** 2))
        print(f'MER {mer:.1f} dB over {len(z) // c.DATA} symbols')
    else:
        print('MER: no symbols (no lock)')

    if ref:
        got = np.fromfile(out, np.uint8)
        want = np.fromfile(ref, np.uint8)
        n = len(got) // 188
        print(f'TS out {len(got)} bytes, {n} packets; sync bytes ok: '
              f'{np.mean(got[:n * 188:188] == 0x47) * 100 if n else 0:.1f}%')
        # find the received packets in the reference (alignment unknown)
        refpk = {bytes(want[i:i + 188]) for i in range(0, len(want) - 187, 188)}
        hits = sum(bytes(got[i * 188:i * 188 + 188]) in refpk for i in range(n))
        print(f'packets matching the transmitted TS: {hits}/{n}')


if __name__ == '__main__':
    main(*sys.argv[1:4])

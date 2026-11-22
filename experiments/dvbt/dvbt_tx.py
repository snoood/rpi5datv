#!/usr/bin/env python3
"""TS file -> DVB-T complex baseband (cf32 at the elementary rate).

    dvbt_tx.py in.ts out.cf32
"""
import sys
from gnuradio import gr, blocks, digital, dtv
import dvbt_common as c


def main(ts, out):
    tb = gr.top_block()
    src = blocks.file_source(gr.sizeof_char, ts, False)
    chain = [
        src,
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
        blocks.file_sink(gr.sizeof_gr_complex, out, False),
    ]
    for a, b in zip(chain, chain[1:]):
        tb.connect(a, b)
    tb.run()


if __name__ == '__main__':
    main(*sys.argv[1:3])

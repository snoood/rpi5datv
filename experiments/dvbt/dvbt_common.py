"""Shared DVB-T parameters for the rpi5datv multi-bit DAC experiment.

Mode: 2k, QPSK, code rate 1/2, guard 1/4, non-hierarchical. The most
robust mode a DVB-T TV tuner supports. Useful TS rate:
    1512 carriers * 2 bit * 1/2 * 188/204 / (Tu + Tg)
    = 4.976 / 4.354 / 3.732 Mbit/s for 8 / 7 / 6 MHz channels
(same as DVB-S at 3600 / 3150 / 2700 kS/s with FEC 3/4).
"""
from gnuradio import dtv

FFT = 2048
CARRIERS = 1705          # occupied tones, 2k mode
DATA = 1512              # data carriers per symbol
CP = FFT // 4            # guard interval 1/4
MODE = dtv.T2k
CONST = dtv.MOD_QPSK
RATE = dtv.C1_2
GI = dtv.GI_1_4


def elem_rate(bw_mhz):
    """DVB-T elementary sample rate (Hz) for a 6/7/8 MHz channel."""
    return bw_mhz * 8e6 / 7


def ts_rate(bw_mhz):
    """Useful TS bit rate (bit/s) for the mode above."""
    tu = FFT / elem_rate(bw_mhz)
    return DATA * 2 * 0.5 * 188 / 204 / (tu * 1.25)

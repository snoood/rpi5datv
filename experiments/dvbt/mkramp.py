#!/usr/bin/env python3
"""Test pattern for ppplay timing checks (4-bit): the LSB toggles every
sample; even turns count up 0..f, odd turns count down f..0, so a capture
shows which state machine produced each sample.
    mkramp.py out.u32 NWORDS [TURN_WORDS]"""
import sys
import numpy as np
n = int(sys.argv[2])
turn = int(sys.argv[3]) if len(sys.argv) > 3 else n
up = np.array([0x76543210, 0xFEDCBA98], np.uint32)
down = np.array([0x89ABCDEF, 0x01234567], np.uint32)
w = np.empty(n, np.uint32)
for t in range(n // turn):
    w[t * turn:(t + 1) * turn] = np.tile(up if t % 2 == 0 else down, turn // 2)
w.astype('<u4').tofile(sys.argv[1])

#!/usr/bin/env python3
"""Dump a multi-bit ppplay capture (PP_CAP_BITS=4): values per PIO cycle
as (value, run length) pairs. pp_dump.py cap.bin BITS [skip_runs] [count]"""
import sys
import numpy as np
w = np.fromfile(sys.argv[1], '<u4')
b = int(sys.argv[2]); skip = int(sys.argv[3]) if len(sys.argv) > 3 else 0
cnt = int(sys.argv[4]) if len(sys.argv) > 4 else 80
per = 32 // b
v = ((w[:, None] >> (np.arange(per, dtype=np.uint32) * b)) & ((1 << b) - 1)).ravel()
e = np.flatnonzero(np.diff(v)) + 1
starts = np.r_[0, e]; lens = np.diff(np.r_[starts, len(v)])
print(' '.join(f'{v[s]:x}:{l}' for s, l in list(zip(starts, lens))[skip:skip + cnt]))

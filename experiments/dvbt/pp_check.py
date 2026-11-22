#!/usr/bin/env python3
"""Check a ppplay capture of the DAC LSB.

    pp_check.py capture.bin TURN_SAMPLES

Play a ramp (LSB toggles every sample) and capture: every sample must
last exactly 2 PIO cycles, i.e. every run of equal bits in the capture
is 2 long. Prints the run-length histogram and where the odd runs are,
in units of samples, and whether they line up with turn boundaries.
"""
import sys
import numpy as np


def main(path, turn):
    turn = int(turn)
    w = np.fromfile(path, '<u4')
    bits = ((w[:, None] >> np.arange(32, dtype=np.uint32)) & 1).astype(np.int8).ravel()
    # skip the idle start (before the kick) and anything stale
    edges = np.flatnonzero(np.diff(bits)) + 1
    if len(edges) < 100:
        print('no activity captured')
        return
    edges = edges[10:]
    runs = np.diff(edges)
    vals, counts = np.unique(runs, return_counts=True)
    print('run lengths (PIO cycles):', dict(zip(vals.tolist(), counts.tolist())))
    odd = np.flatnonzero(runs != 2)
    print(f'{len(runs)} samples checked ({len(runs) / turn:.1f} turns), {len(odd)} odd runs')
    if len(odd):
        pos = (edges[odd] - edges[0]) / 2          # in samples from the first edge
        print('first odd runs (sample index, length):',
              [(round(float(p)), int(runs[i])) for p, i in zip(pos[:12], odd[:12])])
        d = np.diff(pos)
        if len(d):
            print('spacing between odd runs (samples):', np.unique(np.round(d)).tolist()[:12],
                  f'(turn = {turn})')


if __name__ == '__main__':
    main(*sys.argv[1:3])

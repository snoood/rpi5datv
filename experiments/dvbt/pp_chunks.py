#!/usr/bin/env python3
"""Chunk-wise check of a 1-bit ppplay capture against the played file.

    pp_chunks.py capture.bin played.u32 BITS TURN_SAMPLES [CAPTURES_PER_SAMPLE=2]

The capture path itself occasionally drops PIO cycles under DMA load,
which ruins a whole-run comparison. So: decode samples (runs of 2k cycles
-> k samples), cut into chunks, and align each chunk on its own against
the file's sample LSBs. A chunk that matches exactly proves the output
was right there; chunks that span a turn boundary prove the handoff.
"""
import sys
import numpy as np


def unpack(words, bits):
    per = 32 // bits
    return ((words[:, None] >> (np.arange(per, dtype=np.uint32) * bits))
            & ((1 << bits) - 1)).astype(np.uint8).ravel()


def main(cap, played, bits, turn, cyc_per_sample=2, chunk=20000):
    bits, turn, cps = int(bits), int(turn), int(cyc_per_sample)
    c = unpack(np.fromfile(cap, '<u4'), 1)
    e = np.flatnonzero(np.diff(c)) + 1
    starts = np.r_[0, e]
    lens = np.diff(np.r_[starts, len(c)])[1:-1]
    vals = c[starts][1:-1]
    good = (lens % cps == 0) & (lens <= 20 * cps)          # plausible runs only
    ref = unpack(np.fromfile(played, '<u4'), bits) & 1
    n = len(ref)
    # index of the reference by 32-sample keys for fast alignment
    key = lambda a, i: int(np.packbits(a[i:i + 32]).view('>u4')[0])
    table = {}
    for i in range(0, n - 32):
        table.setdefault(key(ref, i), []).append(i)
    ok = bad = unaligned = boundary_ok = 0
    # chunks made of consecutive plausible runs
    run_start = 0
    pos_runs = np.flatnonzero(~good)
    cuts = np.r_[-1, pos_runs, len(lens)]
    for a, b in zip(cuts[:-1], cuts[1:]):
        L, V = lens[a + 1:b], vals[a + 1:b]
        seq = np.repeat(V, L // cps)
        for s in range(0, len(seq) - chunk, chunk):
            part = seq[s:s + chunk]
            cand = table.get(key(part, 0), [])
            hit = None
            for p in cand:
                idx = (p + np.arange(chunk)) % n
                if np.array_equal(ref[idx], part):
                    hit = p
                    break
            if hit is None:
                if cand:
                    bad += 1
                else:
                    unaligned += 1
            else:
                ok += 1
                if (hit % turn) + chunk > turn:        # chunk crosses a handoff
                    boundary_ok += 1
    print(f'{len(lens)} runs, {np.count_nonzero(~good)} implausible (capture glitches)')
    print(f'chunks of {chunk} samples: {ok} exact, {bad} mismatched, {unaligned} unaligned; '
          f'{boundary_ok} exact chunks span a turn handoff')


if __name__ == '__main__':
    main(*sys.argv[1:6])

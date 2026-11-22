#!/usr/bin/env python3
"""Verify a 4-bit ppplay capture (PP_CAP_BITS=4) sample by sample.

    pp_verify.py capture.bin played.u32 BITS [CAP_BITS]

CAP_BITS = 1 (default 4 = BITS): the capture holds only the LSB; it is
compared with the LSBs of the file's samples. (The RP1's RX DMA inserts
zero words into fast 4-bit captures, so 1-bit is the reliable one.) The
first 5000 samples (capture start-up) are skipped.

Each sample must last exactly 2 PIO cycles; the decoded sample sequence
must equal the file's samples in order (turns alternate between the two
state machines, so this checks every handoff). Reports the first
mismatches. Runs of 2k cycles are k equal samples.
"""
import sys
import numpy as np


def unpack(words, bits):
    per = 32 // bits
    return ((words[:, None] >> (np.arange(per, dtype=np.uint32) * bits))
            & ((1 << bits) - 1)).astype(np.uint8).ravel()


def main(cap, played, bits, cap_bits=None):
    bits = int(bits)
    cap_bits = int(cap_bits) if cap_bits else bits
    c = unpack(np.fromfile(cap, '<u4'), cap_bits)
    e = np.flatnonzero(np.diff(c)) + 1
    starts = np.r_[0, e]
    lens = np.diff(np.r_[starts, len(c)])
    vals = c[starts]
    # drop the idle lead-in and the final hold
    starts, lens, vals = starts[1:-1], lens[1:-1], vals[1:-1]
    # skip capture start-up
    cyc = np.cumsum(lens)
    keep = cyc > 10000
    starts, lens, vals = starts[keep][1:], lens[keep][1:], vals[keep][1:]
    oddlen = np.flatnonzero(lens % 2)
    seq = np.repeat(vals, lens // 2)
    ref = unpack(np.fromfile(played, '<u4'), bits)
    if cap_bits == 1:
        ref = ref & 1
    ref2 = np.concatenate([ref, ref])                # allow wrap-around
    # align on a 64-sample window that is unique in the reference
    # probe from the middle (clear of any capture start-up artefacts)
    mid = len(seq) // 2
    probe = seq[mid:mid + 256]
    from numpy.lib.stride_tricks import sliding_window_view
    win = sliding_window_view(ref2[:len(ref) + 256], 256)
    hits = np.flatnonzero((win == probe).all(axis=1)) - mid
    hits = hits % len(ref)
    print(f'{len(seq)} samples decoded, {len(oddlen)} odd-length runs; '
          f'alignment candidates: {len(hits)}')
    if not len(hits):
        print('could not align the capture with the file')
        return
    # the ramp repeats every 2 turns; any candidate with the same phase works
    a = int(hits[0])
    reps = len(seq) // len(ref) + 3
    refN = np.tile(ref, reps)
    n = len(seq)
    bad = np.flatnonzero(seq != refN[a:a + n])
    print(f'compared {n} samples: {len(bad)} mismatches')
    if len(bad):
        print('first mismatches at sample:', bad[:10].tolist())
    if len(oddlen):
        print('odd runs (index, length):', [(int(i), int(lens[i])) for i in oddlen[:10]])
    if not len(bad) and not len(oddlen):
        print('PERFECT: every sample present, in order, exactly 2 PIO cycles long')


if __name__ == '__main__':
    main(*sys.argv[1:5])

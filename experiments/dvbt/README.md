# DVB-T from GPIOs: multi-bit resistor DAC experiment

Can a Pi 5 send DVB-T (what ordinary TV tuners receive) straight from
its GPIO header? Not with the 1-bit DDS that rpi5datv uses for DVB-S:
COFDM needs a multi-level signal. This folder tests the alternative: 3–4
GPIOs driving a resistor DAC, fed with precomputed samples by the RP1
PIO, with the TV channel taken from a sampling image (like the DVB-S
output).

Status: **simulation complete; gap-free playback working on the Pi 500
(two ping-pong state machines); DAC not built yet, so no TV test yet.**
See `DEVELOPMENT_NOTES.md` → *DVB-T experiment* for the numbers,
reasoning and the RP1 pitfalls.

## Signal plan (8 MHz channel)

* DVB-T 2k, QPSK, code rate 1/2, guard 1/4 (most robust mode),
  4.976 Mbit/s TS. Any TS at that rate works; rpi5datv makes one with
  `rpi5datv -t -s 3600 -c 3/4 --ts-out tc8.ts` (same useful rate).
* PIO at 200 MHz / 6 = **33.333 MS/s**, 4 bits per sample (16.7 MB/s).
* Channel placed at IF 7.333 MHz, so the 14th image is at exactly
  **474.000 MHz (UK channel 21)**. `RF=` picks another frequency.
* 2nd-order band-pass noise shaping notches quantisation noise out of
  the channel.
* Loop files are cut to whole DVB-T superframe periods (504 TS packets,
  ×3 for the 8 MHz resampling ratio) so they repeat seamlessly.

## Files

| File | What |
|---|---|
| `dvbt_common.py` | DVB-T mode parameters |
| `dvbt_tx.py` / `dvbt_rx.py` | GNU Radio gr-dtv modulator / reference receiver (with MER and packet check) |
| `dvbt_loop.py` | TS → seamless loop of real IF samples at the PIO rate; `frame` sub-command (DMA-gap hiding by guard-interval deletion; superseded by ping-pong) |
| `dac_sim.py` | `prep`, `pack` (codes → PIO words), `gaps` (simulate PIO stalls), `image` (what a tuner sees at the RF image, with per-bit gain error and pin skew) |
| `quant.c` | N-bit quantiser with band-pass noise shaping |
| `ppplay.c` | **the player**: 2 ping-pong state machines, gap-free (hides the driver's DMA stalls). Debug env knobs in its header comment |
| `dacplay.c` | 1-state-machine player (uses `src/pio_out.c`, now multi-pin) + throughput measurement; has ~16 µs DMA gaps |
| `captest.c`, `ytest.c` | PIO capture / register probes |
| `mkramp.py`, `pp_check.py`, `pp_dump.py`, `pp_verify.py`, `pp_chunks.py` | test patterns and capture analysis for ppplay timing checks |
| `Makefile` | builds the C tools (on the Pi) |

## Make and play a test-card loop (on the Pi)

```bash
make
rpi5datv -t -s 3600 -c 3/4 --ts-out tc8.ts --duration 12
python3 dvbt_loop.py 8 tc8.ts tc8_474.f32               # ~1 min
./quant 4 2 0.22 2.8 tc8_474.f32 tc8_474_4b.u8
python3 dac_sim.py pack 8 tc8_474_4b.u8 4 32 tc8_474_4b.u32
./ppplay tc8_474_4b.u32 22 4 32 6 247520 3600          # GPIO 22-25, gap-free
```

`ppplay FILE GPIO NPINS PULL_BITS DIV TURN_WORDS SECONDS`: TURN_WORDS
must split the file into an even number of turns (247520 = 200 turns
for the 4-bit loop, 198016 for the 3-bit one); DIV must be even. It also
uses GPIO 27 (header pin 13) as its internal start signal, so leave that
pin unconnected. 3-bit loop on GPIO 23–25 (the DAC board's R2–R4):
`./ppplay tc8_474_3b.u32 23 3 30 6 198016 3600`.

On the TV: manual tuning, 474.000 MHz (UK channel 21), bandwidth 8 MHz.

Check it in simulation first (decode through the modelled DAC):

```bash
head -c 60000000 tc8_474_4b.u8 > part.u8                # ~1.8 s is plenty
python3 dac_sim.py image 8 part.u8 4 img.cf32 0.02 50   # 2 % gain error, 50 ps skew
python3 dvbt_rx.py img.cf32 out.ts tc8.ts               # MER + packets
```

## Wiring

`hardware/rpi5datv-dvbt-dac.kicad_sch` (PDF alongside): GPIO 22, 23,
24, 25 = header pins 15, 16, 18, 22 (LSB first) through 8.06k, 4.02k,
2.00k, 1.00k (1 %) into one summing node; 47 nH to ground and 22 pF in
series (high-pass + DC block); coax to the TV's aerial input. GND on
pin 14. Keep the four leads short and **equal length**: timing skew
between bits is the main quality limit (see the notes). Cable only — no
aerial (UHF TV channels are broadcast spectrum).

**Warnings (this kernel):** if an rp1-pio DMA transfer times out (DMA to
a state machine that isn't running), or a *blocking* FIFO put/get hits a
stalled state machine, the RP1 DMA / firmware is stuck and all PIO use
(rpi5datv too) fails until a reboot. `ppplay` guards against both.

# rpi5datv

A DVB-S digital television transmitter for the **Raspberry Pi 5 / Pi 500**,
in a single executable. It takes a video file, a live stream (named pipe,
stdin or network URL) or a built-in test card, encodes it to H.264 on the
fly, multiplexes it into a DVB transport stream, applies
DVB-S channel coding and puts the QPSK-modulated RF signal straight out of
a GPIO pin using the Pi 5's RP1 PIO.

Current version: **1.9.0 "Bounce On My Boys Buffer"**. 

It is a 64-bit rewrite of [rpidatv](https://github.com/F5OEO/rpidatv)
(F5OEO), which targeted the Pi 3.

## Requirements

- Raspberry Pi 5, Pi 500 or Compute Module 5
- 64-bit Raspberry Pi OS (Bookworm or later), kept up to date
- Internet access for the installer (apt packages)

## Install

```bash
git clone <this repository> rpi5datv
cd rpi5datv
./install.sh
```

Run it as your normal user, not with `sudo`; it calls `sudo` itself when
needed. It is safe to re-run. The installer:

1. installs the build dependencies (FFmpeg development libraries, compiler)
2. builds `bin/rpi5datv` and runs the self tests
3. checks the H.264/MP2 encoders work
4. installs the binary to `/usr/local/bin/rpi5datv` (with permission to
   use real-time scheduling)
5. sets up access to the PIO device `/dev/pio0` (udev rule and the `gpio`
   group), checks the bootloader EEPROM, and checks `config.txt` for
   overlays that could clash with the RF pin

Options:

| Option | Effect |
|---|---|
| `--eeprom` | also apply a pending bootloader EEPROM update |
| `--no-deps` | skip the apt step |
| `--prefix=DIR` | install somewhere other than `/usr/local` |

**Log out and back in (or reboot)** after the first install so the `gpio`
group applies. If the installer says `/dev/pio0` is missing, run
`sudo apt full-upgrade` and `./install.sh --eeprom`, then reboot.

### Building by hand

```bash
sudo apt install build-essential pkg-config libavcodec-dev libavformat-dev \
    libavutil-dev libswscale-dev libswresample-dev
make -C src          # produces bin/rpi5datv
make -C src test     # self tests
```

## Hardware

The RF comes out on **GPIO 12 (header pin 32)** by default; ground is on
pin 30 (or any GND pin). Use `-x` to pick another GPIO (0–27). For local
bench tests a few centimetres of wire or a dummy load into an SDR is
enough.

The output is weak (well below a milliwatt on 70 cm) and is an *image*
of a synthesised carrier, so filter it and add an amplifier for anything
beyond the bench.

## Usage

```
rpi5datv (-i FILE | -t) -f MHZ [options]
```

### Examples

Check the RF plan for a frequency first (nothing is transmitted):

```bash
rpi5datv --plan -f 437 -s 333
```

Transmit the test card on 437 MHz, 333 kS/s, FEC 1/2:

```bash
rpi5datv -t -f 437 -s 333 -c 1/2 -n G0ABC -p "My station"
```

Transmit a video file in a loop at 1000 kS/s, FEC 3/4:

```bash
rpi5datv -i film.mp4 -l -f 437 -s 1000 -c 3/4 -n G0ABC
```

Stop with **Ctrl-C**. The RF pin is released (and PTT dropped) on exit.

### Options

| Option | Meaning | Default |
|---|---|---|
| `-i, --input FILE` | video file, named pipe, `pipe:0` (stdin) or stream URL to send (MP4 H.264/AAC preferred for files; anything FFmpeg can decode works) | |
| `-t, --testcard` | send the built-in test card (overrides `-i`) | |
| `-l, --loop` | loop the video file | off |
| `-f, --frequency MHZ` | RF output frequency in MHz | required |
| `-s, --symbol-rate KS` | symbol rate, 125–4000 kS/s | 333 |
| `-c, --fec RATE` | `1/2`, `2/3`, `3/4`, `5/6`, `7/8` | 1/2 |
| `-n, --service NAME` | DVB service name (usually your callsign) | RPI5DATV |
| `-p, --provider NAME` | DVB service provider name | RPI5DATV |
| `-x, --gpio N` | GPIO for the RF output | 12 |
| `-P, --ptt-gpio N` | GPIO driven high while transmitting | none |
| `-d, --power 0..3` | GPIO drive strength: 2/4/8/12 mA | 3 |
| `-a, --no-audio` | send no audio track | audio on |
| `-W, --size WxH` | picture size | from bit rate |
| `-v, --verbose` | status line every 2 s, encoder logs | |
| `-V, --version` | print the version | |
| `-h, --help` | help | |

Advanced options:

| Option | Meaning | Default |
|---|---|---|
| `--plan` | print the frequency plan and exit | |
| `--pio-div N` | force the PIO clock divider (sample rate = 200 MHz / N) | automatic |
| `--pio-max-rate MBS` | maximum data rate to the PIO for automatic choice | 9 MB/s |
| `--xfer-kb N` | PIO DMA buffer size in KiB (falls back to smaller sizes if the kernel refuses) | 63 |
| `--pio-block-kb N` | data per DMA call in KiB; bigger = fewer tiny gaps in the RF | 1024 |

Test outputs. Any of these turns the RF off, so you can run it on any PC:

| Option | Writes |
|---|---|
| `--ts-out FILE` | the transport stream being sent (playable in VLC/ffplay) |
| `--iq-out FILE` | complex float32 baseband QPSK (`--iq-sps N` samples/symbol, default 4) |
| `--rf-out FILE` | the raw bit stream that would go to the PIO |
| `--duration SEC` | stop after SEC seconds of signal |

```bash
rpi5datv -t -s 333 --ts-out test.ts --duration 10 && ffplay test.ts
```

### Live input (pipes and network streams)

Video is decoded and re-encoded **on the fly**, in step with the RF: there
is no pre-encoding pass and no intermediate file. Whatever you give to
`-i` is opened by FFmpeg, so besides files it accepts a named pipe
(FIFO), standard input (`pipe:0`) or a network URL. This lets you feed
live video from another program, a camera or another machine.

The input is always re-encoded to fit the channel, so its own codec, size,
frame rate and bit rate don't matter.

**Named pipe** (a second program writes, rpi5datv reads):

```bash
mkfifo /tmp/tx.fifo
rpi5datv -i /tmp/tx.fifo -f 437 -s 333 -n G0ABC &
ffmpeg -re -i film.mkv -c:v libx264 -preset ultrafast -c:a aac -f mpegts -y /tmp/tx.fifo
```

**Standard input:**

```bash
ffmpeg -re -i film.mkv -c:v libx264 -preset ultrafast -c:a aac -f mpegts - \
    | rpi5datv -i pipe:0 -f 437 -s 333 -n G0ABC
```

**Pi camera** (untested):

```bash
rpicam-vid -t 0 --width 1280 --height 720 --framerate 25 \
    --codec libav --libav-format mpegts -o - \
    | rpi5datv -i pipe:0 -f 437 -s 333 -n G0ABC
```

**Network stream** from OBS, ffmpeg or a camera on another machine
(untested; Raspberry Pi OS's FFmpeg includes these protocols):

```bash
rpi5datv -i udp://@:1234 -f 437 -s 333 -n G0ABC           # MPEG-TS over UDP
rpi5datv -i "srt://0.0.0.0:9000?mode=listener" -f 437 -s 333 -n G0ABC
rpi5datv -i rtmp://server/live/key -f 437 -s 333 -n G0ABC
```

Try a live source without RF first with `--ts-out test.ts --duration 10`,
then play `test.ts`.

Gotchas:

- **Use a streamable container.** MPEG-TS is the safest; Matroska, FLV and
  NUT also work. Plain MP4 does not work through a pipe (its index is
  written at the end) unless it is fragmented. A raw `.h264` stream has
  no timestamps, so use MPEG-TS instead.
- **Use `pipe:0` for stdin**, not `-` (that shortcut belongs to the
  `ffmpeg` command, not the library rpi5datv uses).
- **Send at real time.** When feeding a file through a pipe, give the
  sending `ffmpeg` `-re`. Without it the sender runs ahead and is simply
  held back by the pipe, which works but the pipe fills up with seconds
  of video and the delay grows.
- **No `-l` (loop)** with pipes or streams: they can't seek back to the
  start.
- **Start order.** Either program can be started first: a writer to a
  FIFO blocks until rpi5datv opens it, and rpi5datv waits up to 15 s for
  the source before starting the signal. If the source takes longer, it
  sends null packets until the video arrives.
- **When the source ends**, rpi5datv ends too. If the source stalls,
  rpi5datv keeps the carrier up with null packets (`underrun-nulls` in the
  status line) until data comes back.
- **The source program dies with "Broken pipe"** when rpi5datv stops.
  That's expected.
- **Delay** from the source to the RF is about 1.5–2 s (muxer, start-up
  buffer and PIO queue), plus whatever the sender adds.
- **Clock drift.** The source's clock and the Pi's transmit clock differ
  slightly (typically < 50 ppm, about 0.2 s an hour). Video copes because
  frames are re-timed to 25 fps; very long live runs (hours) are not yet
  tested.
- **CPU.** Decoding a big source (e.g. 1080p) and encoding at the same
  time uses more CPU. If `underrun-nulls` keep climbing, have the sender
  scale down first (e.g. `-vf scale=1024:576`).

### Picture and sound

The picture is 16:9 at 25 fps. The size depends on the bit rate
available:

| Video bit rate | Picture size |
|---|---|
| ≥ 1.2 Mbit/s | 1024×576 |
| ≥ 600 kbit/s | 768×432 |
| ≥ 300 kbit/s | 640×360 |
| ≥ 150 kbit/s | 512×288 |
| lower | 384×216 |

Other aspect ratios are letter- or pillar-boxed.

Audio is MPEG-1 Layer II at 48 kHz: mono 64 kbit/s, or stereo 128 kbit/s
when the transport stream rate is 1.5 Mbit/s or more. Audio is dropped
below 250 kbit/s. The test card sends a 1 kHz tone.

The transport stream rate is `SR × 2 × FEC × 188/204`. For example,
333 kS/s at FEC 1/2 gives 307 kbit/s.

### Reading the output

At start-up it prints the RF plan:

```
RF plan: 437.000000 MHz = 7 x 66.667 MHz - 29.666667 MHz (inverted image, compensated)
         PIO divider 3, 66.7 Msamples/s, 8.33 MB/s to the PIO, 200.2 samples/symbol
         image level -26.4 dB relative to a square wave at the target frequency
         simulated MER 40.4 dB
```

- **MER** is the expected signal quality; anything above 20 dB is
  comfortable for DVB-S.
- If MER is low, rpi5datv prints a warning. Try a lower symbol rate, a
  slightly different frequency, or `--pio-max-rate 13` if your Pi keeps up.
- With `-v`, a `speed` below x1.000 in the status line means the Pi
  cannot stream data to the PIO fast enough, so the signal has gaps.
  Lower `--pio-max-rate` or reduce the CPU load.
- `underrun-nulls` means the video encoder fell behind; null packets are
  sent so the carrier never drops.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Cannot open /dev/pio0` | Not a Pi 5, an old kernel/EEPROM (`sudo apt full-upgrade`, `./install.sh --eeprom`, reboot), or not in the `gpio` group (log out and back in) |
| `FFmpeg development libraries not found` when building | run `./install.sh`, or install the `lib*-dev` packages listed above |
| `No H.264 encoder available` | libavcodec without libx264; Raspberry Pi OS's FFmpeg includes it |
| Live input: nothing happens / `Invalid data found` | the writer hasn't started, or the format isn't streamable: send MPEG-TS (`-f mpegts`) |
| Live input: `underrun-nulls` keep rising | the source is slower than real time or the Pi is overloaded: scale the source down before sending |
| Receiver does not lock | check frequency, SR and FEC match; check `--plan` MER; make sure the receiver isn't overloaded (the GPIO is very close) |

## Layout

```
install.sh            installer
bin/rpi5datv          built executable
src/                  source (main, source/encoder, DVB-S coder, modulator, PIO output, test card)
src/vendor/piolib/    Raspberry Pi's piolib (BSD-3)
src/tests/            self tests (make -C src test)
```


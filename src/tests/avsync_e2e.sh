#!/bin/bash
#
# rpi5datv end-to-end A/V sync test (no RF).
#
# Makes test streams with ffmpeg in which a white video flash and a 1 kHz
# audio beep happen together every 2 s, damages them in the ways live
# input gets damaged, runs them through rpi5datv (decode -> re-encode ->
# mux, --ts-out) and checks that every flash still has its beep within
# LIMIT_MS in the output.
#
#   tests/avsync_e2e.sh [path/to/rpi5datv]      (or: make -C src e2e)
#
# Needs ffmpeg and ffprobe with libx264 (Raspberry Pi OS has them).
# Works anywhere; it never touches the PIO. Exit status 0 = all passed.

set -u
BIN=${1:-$(dirname "$0")/../../bin/rpi5datv}
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
LIMIT_MS=60
for t in ffmpeg ffprobe; do
    command -v $t >/dev/null || { echo "SKIP: $t not installed"; exit 0; }
done
[ -x "$BIN" ] || { echo "no rpi5datv binary at $BIN (build first)"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cd "$W"

V="color=c=black:s=320x180:r=25,drawbox=c=white:t=fill:enable='lt(mod(t\,2)\,0.04)'"
A="aevalsrc='if(lt(mod(t\,2)\,0.04)\,sin(2*PI*1000*t)\,0)':s=48000"
gen() { # out seconds [video filter] [audio filter]
    ffmpeg -loglevel error -f lavfi -i "$V" -f lavfi -i "$A" -t "$2" \
        -vf "${3:-null}" -af "${4:-anull}" -fps_mode passthrough \
        -c:v libx264 -preset ultrafast -g 25 -c:a mp2 -b:a 128k -f mpegts -y "$1"
}

echo "Making test streams in $W"
gen clean.ts 20
gen gap.ts 20 null "aselect='not(between(t\,5\,5.6))'"          # 0.6 s audio lost
gen seg.ts 10
cat seg.ts seg.ts > jump.ts                                      # timestamps restart
gen early.ts 20 "trim=start=1"                                   # audio 1 s before video
# slideshow: a white + a black picture every 4 s (gaps of 3.96 s, longer
# than the 3 s jump threshold; the continuous audio must keep them)
gen slide.ts 20 "select='lt(mod(n\,100)\,2)'"

# Flash and beep times on ONE time base (ffmpeg normalises both streams
# to the same zero), then the beep-minus-flash offset for each flash.
measure() { # file -> lines "flash_time offset_ms"
    {
        ffmpeg -nostats -i "$1" -map 0:v -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG \
            -f null - 2>&1 | awk '/pts_time/{match($0,/pts_time:[0-9.]+/); t=substr($0,RSTART+9,RLENGTH-9)}
                /YAVG/{split($0,a,"="); y=a[2]+0; if (y>100 && !on) print "V", t; on=(y>100)}'
        ffmpeg -nostats -i "$1" -map 0:a -af silencedetect=n=-30dB:d=0.01 -f null - 2>&1 \
            | sed -n 's/.*silence_end: \([0-9.]*\).*/A \1/p'
    } | awk '$1=="V"{v[nv++]=$2} $1=="A"{a[na++]=$2}
        END{for(i=0;i<nv;i++){b=1e9; for(j=0;j<na;j++){d=a[j]-v[i]; if((d<0?-d:d)<(b<0?-b:b)) b=d}
            printf "%.2f %.0f\n", v[i], b*1000}}'
}

fails=0
run() { # name input min_flashes [extra rpi5datv args]
    local name=$1 in=$2 want=$3; shift 3
    "$BIN" -i "$in" -s 1000 -c 3/4 --ts-out "out_$name.ts" --duration 19 -v "$@" \
        > "log_$name.txt" 2>&1
    local res; res=$(measure "out_$name.ts")
    # the last flash may have lost its beep to the --duration cut-off
    local body; body=$(echo "$res" | sed '$d')
    local nfl; nfl=$(echo "$res" | grep -c .)
    local worst; worst=$(echo "$body" | awk '{o=$2<0?-$2:$2; if(o>w) w=o} END{print w+0}')
    local notes; notes=$(grep -cE "jumped|behind|ahead" "log_$name.txt")
    local ok=1
    [ "$nfl" -ge "$want" ] || ok=0
    [ "$worst" -le "$LIMIT_MS" ] || ok=0
    printf "  %-8s %-4s flashes %2d (want >= %2d), worst offset %4s ms, corrections %s\n" \
        "$name" "$([ $ok = 1 ] && echo ok || echo FAIL)" "$nfl" "$want" "$worst" "$notes"
    [ $ok = 1 ] || { fails=$((fails+1)); echo "$res" | sed 's/^/           /'; }
}

echo "A/V sync end-to-end ($BIN, limit +/-${LIMIT_MS} ms):"
run clean clean.ts 9
run gap   gap.ts   9
run jump  jump.ts  9
run early early.ts 9
run slide slide.ts 4
run loop  seg.ts   9 -l

if [ $fails -eq 0 ]; then echo "A/V SYNC END-TO-END PASSED"; else echo "A/V SYNC END-TO-END: $fails FAILED"; fi
exit $((fails > 0))

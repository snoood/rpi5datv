#!/bin/bash
#
# rpi5datv installer for Raspberry Pi 5 / Pi 500 (64-bit Raspberry Pi OS).
#
#   ./install.sh            install dependencies, build, set up PIO access
#   ./install.sh --no-deps  skip apt (build + system setup only)
#   ./install.sh --eeprom   also apply any pending bootloader EEPROM update
#   ./install.sh --help
#
# Safe to re-run. Uses sudo for system changes; run as your normal user.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="/usr/local"
DO_DEPS=1
DO_EEPROM=0
REBOOT_NEEDED=0
RELOGIN_NEEDED=0

info()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mWARNING:\033[0m %s\n' "$*" >&2; }
die()   { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

for arg in "$@"; do
    case "$arg" in
        --no-deps) DO_DEPS=0 ;;
        --eeprom)  DO_EEPROM=1 ;;
        --prefix=*) PREFIX="${arg#--prefix=}" ;;
        -h|--help)
            sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "unknown option '$arg' (try --help)" ;;
    esac
done

[ "$(id -u)" -ne 0 ] || die "run this as your normal user (it calls sudo itself)"
TARGET_USER="$(id -un)"

# ------------------------------------------------------------------ checks
info "Checking the platform"
ARCH="$(uname -m)"
MODEL="$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)"
echo "    model: $MODEL"
echo "    arch:  $ARCH"
echo "    kernel: $(uname -r)"

IS_PI5=0
case "$MODEL" in
    *"Raspberry Pi 5"*|*"Raspberry Pi 500"*|*"Compute Module 5"*) IS_PI5=1 ;;
esac
if [ "$ARCH" != "aarch64" ]; then
    warn "not a 64-bit ARM system - building anyway, but RF output needs a Pi 5/500 running 64-bit Raspberry Pi OS"
fi
if [ $IS_PI5 -eq 0 ]; then
    warn "this does not look like a Raspberry Pi 5/500; the RF output uses the RP1 PIO which only those have"
fi

# ------------------------------------------------------------- packages
if [ $DO_DEPS -eq 1 ]; then
    info "Installing build dependencies (apt)"
    sudo apt-get update
    sudo apt-get install -y build-essential pkg-config \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
        libcap2-bin
fi

# ---------------------------------------------------------------- build
info "Building rpi5datv"
make -C "$REPO_DIR/src" -j"$(nproc)"
info "Running self tests"
make -C "$REPO_DIR/src" test

info "Checking the H.264 / MP2 encoders work"
SMOKE_TS="$(mktemp --suffix=.ts)"
if "$REPO_DIR/bin/rpi5datv" -t -s 333 -c 1/2 --ts-out "$SMOKE_TS" --duration 2 >/dev/null; then
    echo "    ok ($(stat -c %s "$SMOKE_TS") bytes of transport stream)"
else
    rm -f "$SMOKE_TS"
    die "encoder smoke test failed - is libavcodec built with libx264? (Raspberry Pi OS's is)"
fi
rm -f "$SMOKE_TS"

info "Installing to $PREFIX/bin/rpi5datv"
sudo install -D -m 0755 "$REPO_DIR/bin/rpi5datv" "$PREFIX/bin/rpi5datv"
# Allow SCHED_FIFO for the modulator thread without running as root.
if command -v setcap >/dev/null; then
    sudo setcap cap_sys_nice+ep "$PREFIX/bin/rpi5datv" || warn "setcap failed; real-time priority needs sudo"
fi

# ------------------------------------------------------ PIO device access
if [ $IS_PI5 -eq 1 ]; then
    info "Setting up access to the RP1 PIO (/dev/pio0)"

    # The rp1-pio driver lands in kernel 6.6.x+ (2024-11 onward); RP1 PIO
    # also needs bootloader EEPROM firmware from late 2024.
    if command -v rpi-eeprom-update >/dev/null; then
        EEPROM_STATUS="$(sudo rpi-eeprom-update 2>&1 || true)"
        echo "$EEPROM_STATUS" | sed 's/^/    /'
        if echo "$EEPROM_STATUS" | grep -q "UPDATE AVAILABLE"; then
            if [ $DO_EEPROM -eq 1 ]; then
                sudo rpi-eeprom-update -a
                REBOOT_NEEDED=1
            else
                warn "a bootloader EEPROM update is available; re-run with --eeprom (or 'sudo rpi-eeprom-update -a') if /dev/pio0 is missing"
            fi
        fi
    fi

    RULE=/etc/udev/rules.d/99-rpi5datv-pio.rules
    if [ ! -f "$RULE" ]; then
        echo 'SUBSYSTEM=="*-pio", GROUP="gpio", MODE="0660"' | sudo tee "$RULE" >/dev/null
        sudo udevadm control --reload-rules
        sudo udevadm trigger --subsystem-match='*-pio' || true
        echo "    added $RULE"
    fi
    if ! getent group gpio >/dev/null; then
        sudo groupadd --system gpio
    fi
    if ! id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx gpio; then
        sudo usermod -aG gpio "$TARGET_USER"
        RELOGIN_NEEDED=1
        echo "    added $TARGET_USER to group gpio"
    fi

    if [ -e /dev/pio0 ]; then
        echo "    /dev/pio0: $(ls -l /dev/pio0)"
    else
        warn "/dev/pio0 does not exist yet. Update the system ('sudo apt full-upgrade'),"
        warn "apply the EEPROM update (./install.sh --eeprom) and reboot."
        REBOOT_NEEDED=1
    fi

    # ------------------------------------------------------ config.txt
    # No overlay is needed: the PIO claims its GPIO at run time through
    # the rp1-pio driver. We only check nothing else already owns the
    # default RF pin (GPIO 12, header pin 32).
    CONFIG=/boot/firmware/config.txt
    if [ -f "$CONFIG" ]; then
        info "Checking $CONFIG"
        if grep -Eq '^\s*dtoverlay=(pwm|pwm-2chan|pwm-pio|audremap)' "$CONFIG"; then
            warn "$CONFIG enables a PWM/audio overlay that may claim GPIO 12/13/18/19."
            warn "Either remove it or choose another RF pin with -x."
        else
            echo "    nothing to change (no overlay needed for the RP1 PIO)"
        fi
    fi
fi

# ------------------------------------------------------------------ done
echo
info "Done."
cat <<EOF

  Try it (look at the plan first, no RF):
      rpi5datv --plan -f 437 -s 333

  Transmit the test card on GPIO 12 (header pin 32):
      rpi5datv -t -f 437 -s 333 -c 1/2 -n YOURCALL -p "Your name"

  Transmit a video file, looped:
      rpi5datv -i video.mp4 -l -f 437 -s 1000 -c 3/4 -n YOURCALL

EOF
if [ $RELOGIN_NEEDED -eq 1 ]; then
    echo
    warn "log out and back in (or reboot) so the new 'gpio' group membership applies"
fi
if [ $REBOOT_NEEDED -eq 1 ]; then
    echo
    warn "a reboot is needed before transmitting"
fi

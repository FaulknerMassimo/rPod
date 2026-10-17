#!/bin/sh
# Base system for rPod (docs/PLAN.md §3) on a fresh Raspberry Pi OS Lite
# 64-bit: packages, pigpio, boot config, MPD and rpod's own paths, then
# Bluetooth audio (bluetooth/setup.sh) and the PiSugar 2 (pisugar/setup.sh). Everything the deploy-* targets
# install on top of it needs this first.
#
# Runs on the Pi as root, from a copy of the repo's system/ directory --
# `make pi-setup` rsyncs it over and runs this. Safe to re-run. Boot config
# only takes effect after a reboot; this says when it changed it.
set -eu

cd "$(dirname "$0")"

RPOD_USER=${RPOD_USER:-rpod}
# No terminal: `make pi-setup` runs this as a systemd unit.
export DEBIAN_FRONTEND=noninteractive
model=$(tr -d '\0' </proc/device-tree/model)
echo "== $model"

# --- Packages (PLAN §3) ------------------------------------------------------
#
# The -dev packages are for the dev machine's cross builds, not the Pi:
# `make sysroot` copies their headers and libraries off it. Recommends are
# skipped, as RAM and the SD card's write budget matter more than docs.
apt-get update
apt-get install -y --no-install-recommends \
    build-essential pkg-config git device-tree-compiler rsync \
    libdrm-dev libmpdclient-dev libcurl4-openssl-dev libsystemd-dev \
    libjpeg62-turbo-dev zlib1g-dev \
    libavformat-dev libavcodec-dev libavfilter-dev libswscale-dev libavutil-dev \
    mpd mpc exfatprogs

# --- pigpio (PLAN §4.5) ------------------------------------------------------
#
# Not packaged (apt only has the pigpiod_if2 client split), so built from
# source here, into /usr/local: rpod-wheel and wheel-sniff link it, and
# `make sysroot` copies it for the cross build.
if [ ! -e /usr/local/lib/libpigpio.so ]; then
    src=/usr/local/src/pigpio
    [ -d "$src" ] || git clone --depth 1 --branch v79 https://github.com/joan2937/pigpio "$src"
    make -C "$src" -j4
    # Its install also tries the Python module, which needs setuptools this
    # OS doesn't ship; the C library is installed first either way.
    make -C "$src" install || test -e /usr/local/lib/libpigpio.so
    ldconfig
fi

# --- Boot config (PLAN §3, system/config.txt.d/rpod.txt) --------------------
#
# The fragment goes at the end of config.txt between markers, replacing any
# earlier copy, so a re-run picks up changes to it. The Pi 3B dev board keeps
# its headphone jack (the fragment's header). The originals are kept beside
# them as *.orig, once.
boot=/boot/firmware
for f in config.txt cmdline.txt; do
    [ -e "$boot/$f.orig" ] || cp "$boot/$f" "$boot/$f.orig"
done
cp "$boot/config.txt" /tmp/config.txt.before
cp "$boot/cmdline.txt" /tmp/cmdline.txt.before

sed -i '/^# >>> rpod/,/^# <<< rpod/d' "$boot/config.txt"
{
    echo '# >>> rpod: system/config.txt.d/rpod.txt, installed by system/setup.sh'
    echo '[all]'
    case $model in
        *"Pi 3 Model B"*) sed 's/^dtparam=audio=off$/dtparam=audio=on/' config.txt.d/rpod.txt ;;
        *) cat config.txt.d/rpod.txt ;;
    esac
    echo '# <<< rpod'
} >>"$boot/config.txt"

# fbcon=map:9 keeps the kernel console off the panel's framebuffer
# whichever /dev/fbN it gets; quiet and logo.nologo are for boot time.
for arg in fbcon=map:9 quiet logo.nologo; do
    grep -qw -- "$arg" "$boot/cmdline.txt" ||
        sed -i "1s/\$/ $arg/" "$boot/cmdline.txt"
done

# The panel's overlay (system/overlays/rpod-panel.dts), which the fragment
# loads after fbtft's. `make deploy-overlay` updates just this.
dtc -@ -q -I dts -O dtb -o /tmp/rpod-panel.dtbo overlays/rpod-panel.dts
cmp -s /tmp/rpod-panel.dtbo "$boot/overlays/rpod-panel.dtbo" || {
    install -m 644 /tmp/rpod-panel.dtbo "$boot/overlays/"
    touch /tmp/rpod-panel.changed
}

# Boot time (PLAN §3): nothing on rPod waits for the network, and nothing
# should run apt or rebuild man pages behind the UI's back.
for unit in NetworkManager-wait-online.service systemd-networkd-wait-online.service \
            apt-daily.timer apt-daily-upgrade.timer man-db.timer triggerhappy.service; do
    if systemctl cat "$unit" >/dev/null 2>&1; then
        systemctl disable --now "$unit" 2>/dev/null || true
    fi
done
# Swap: Raspberry Pi OS's rpi-swap already sets up zram, so PLAN §3's
# zram-tools isn't needed on it.

# --- MPD (PLAN §6.1) ---------------------------------------------------------
#
# Debian's mpd.service, with mpd.socket masked: it would also listen on TCP
# 6600, and rPod wants the Unix socket only. /etc/mpd.conf is
# system/mpd/mpd.conf, installed by bluetooth/setup.sh below.
systemctl disable --now mpd.socket 2>/dev/null || true
systemctl mask mpd.socket
systemctl enable mpd.service

# --- rPod's own paths --------------------------------------------------------
#
# The library and the videos beside it (PLAN §8.7), the rpod user's so web
# uploads can write there, readable by mpd. Left alone if they're there.
for dir in /media/music /media/videos; do
    [ -d "$dir" ] || install -d -o "$RPOD_USER" -g "$RPOD_USER" -m 755 "$dir"
done
# Overrides for rpod, rpod-wheel and rpod-web (their units' EnvironmentFile).
# Root-only: it can hold RPOD_WEB_PASSWORD. Never overwritten.
if [ ! -e /etc/rpod/env ]; then
    install -d -m 755 /etc/rpod
    install -m 600 /dev/null /etc/rpod/env
    echo 'RPOD_FB=/dev/rpod-panel' >/etc/rpod/env
fi

# --- Bluetooth audio (PLAN §6.3) ---------------------------------------------
sh bluetooth/setup.sh

# --- The PiSugar 2: battery, power, RTC (PLAN §7.6) --------------------------
sh pisugar/setup.sh

echo
if ! cmp -s /tmp/config.txt.before "$boot/config.txt" ||
   ! cmp -s /tmp/cmdline.txt.before "$boot/cmdline.txt" ||
   [ -e /tmp/rpod-panel.changed ]; then
    echo "== Boot config changed: reboot (sudo reboot), then make sysroot and deploy."
else
    echo "== Boot config unchanged."
fi
rm -f /tmp/config.txt.before /tmp/cmdline.txt.before /tmp/rpod-panel.changed

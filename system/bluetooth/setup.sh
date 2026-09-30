#!/bin/sh
# Bluetooth audio for rPod (docs/PLAN.md §6.3): BlueZ, plus a system-wide
# PipeWire + WirePlumber that MPD's "Bluetooth" output plays into.
#
# Runs on the Pi as root, from a copy of the repo's system/ directory --
# `make bluetooth-setup` rsyncs it over and runs this. Safe to re-run.
set -eu

cd "$(dirname "$0")/.."

# --no-install-recommends skips pipewire-pulse, rtkit and friends: nothing on
# rPod talks PulseAudio, and RAM is the binding constraint (docs/PLAN.md §0).
apt-get update
apt-get install -y --no-install-recommends \
    bluez pipewire wireplumber libspa-0.2-bluetooth

# Debian ships no system-mode PipeWire, so nothing creates the user the
# upstream system units run as.
getent group pipewire >/dev/null || groupadd --system pipewire
getent passwd pipewire >/dev/null ||
    useradd --system --gid pipewire --home-dir /nonexistent --no-create-home \
            --shell /usr/sbin/nologin pipewire
# bluetooth: BlueZ's D-Bus policy, for WirePlumber's A2DP endpoints.
# pipewire: lets MPD connect to /run/pipewire/pipewire-0 (mode 0660).
usermod -a -G bluetooth pipewire
usermod -a -G pipewire mpd

install -m 644 systemd/pipewire.socket systemd/pipewire-manager.socket \
               systemd/pipewire.service systemd/wireplumber.service \
               /etc/systemd/system/
install -D -m 644 wireplumber/wireplumber.conf.d/rpod.conf \
                  /etc/wireplumber/wireplumber.conf.d/rpod.conf
diff -u /etc/mpd.conf mpd/mpd.conf || true
install -m 644 mpd/mpd.conf /etc/mpd.conf

# Debian enables the per-user session units for every login. Left alone, any
# PipeWire client run over SSH (wpctl, pw-cli, ...) would socket-activate a
# second PipeWire + WirePlumber for that user -- one whose ALSA monitor grabs
# the sound card MPD plays through.
systemctl --global mask pipewire.socket pipewire.service wireplumber.service

if command -v rfkill >/dev/null; then
    rfkill unblock bluetooth
fi

systemctl daemon-reload
systemctl enable bluetooth.service pipewire.socket pipewire-manager.socket \
                 pipewire.service wireplumber.service
systemctl start bluetooth.service pipewire.socket pipewire-manager.socket
# restart, not start, so a re-run picks up changed units/config -- and MPD
# only reads its audio_output blocks at startup.
systemctl restart pipewire.service wireplumber.service mpd.service
sleep 2

echo
echo "== Bluetooth controller"
timeout 5 bluetoothctl show || true
echo
echo "== PipeWire (headset sinks show up under Audio > Sinks once connected)"
PIPEWIRE_RUNTIME_DIR=/run/pipewire wpctl status || true
echo
echo "== MPD outputs"
mpc -h /run/mpd/socket outputs || true

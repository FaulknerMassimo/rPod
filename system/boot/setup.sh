#!/bin/sh
# Boot time (docs/PLAN.md §3): what Raspberry Pi OS runs at boot that rPod
# doesn't need, swap without the SD card, MPD not waiting for the network
# and NetworkManager waiting for rPod, the boot partition mounted on use,
# and the kernel kept uncompressed. The boot config's half (no initramfs, no
# camera or display probing, the kernel's command line) is system/setup.sh's,
# and the logo shown meanwhile is rpod-splash.service, installed with rpod.
#
# Runs on the Pi as root, from a copy of the repo's system/ directory --
# system/setup.sh runs it, and `make boot-setup` runs just this. Safe to
# re-run. Takes effect at the next boot.
set -eu

cd "$(dirname "$0")"

# Nothing on rPod waits for the network, and nothing should run apt or
# rebuild man pages behind the UI's back. keyboard-setup loads a console
# keymap for a keyboard rPod doesn't have, and e2scrub_reap cleans up after
# LVM snapshots it doesn't use; both sit early in the boot.
for unit in NetworkManager-wait-online.service systemd-networkd-wait-online.service \
            apt-daily.timer apt-daily-upgrade.timer man-db.timer triggerhappy.service \
            keyboard-setup.service e2scrub_reap.service; do
    if systemctl cat "$unit" >/dev/null 2>&1; then
        systemctl disable --now "$unit" 2>/dev/null || true
    fi
done

# cloud-init applied Raspberry Pi Imager's settings on the first boot (the
# user, SSH, Wi-Fi -- netplan keeps that one, not cloud-init). On every boot
# since, it held systemd up for seconds before its services ran ahead of
# everything else. The marker file switches its services off; masking its
# generator (a shell script that still ran on every boot) saves the rest.
mkdir -p /etc/systemd/system-generators
if [ -d /etc/cloud ]; then
    touch /etc/cloud/cloud-init.disabled
    ln -sfn /dev/null /etc/systemd/system-generators/cloud-init-generator
fi

# Swap is zram alone (PLAN §3), set up by zram-generator itself.
# Raspberry Pi OS's rpi-swap does that through a generator that's a Python
# script -- 1.5 s on every boot, and again on every daemon-reload, which
# NetworkManager runs several of at boot -- and backs zram with a swap file
# on the SD card, attached and resized on the boot's critical path. Same
# size and compression as it chose, minus the card.
if [ -e /usr/lib/systemd/system-generators/rpi-swap-generator ]; then
    ln -sfn /dev/null /etc/systemd/system-generators/rpi-swap-generator
    rm -f /var/swap
fi
# A drop-in, as rpi-swap's own (20-rpi-swap-zram0-ctrl.conf) switches zram0
# off unless its generator switches it back on: this one comes after it.
mkdir -p /etc/systemd/zram-generator.conf.d
cat >/etc/systemd/zram-generator.conf.d/90-rpod.conf <<'EOF'
# rPod's swap: compressed RAM, never the SD card (system/boot/setup.sh).
[zram0]
host-memory-limit = none
fs-type = swap
zram-size = ram
compression-algorithm = zstd
swap-priority = 100
EOF
# What rpi-swap set for it: no readahead, pointless for RAM.
echo 'vm.page-cluster = 0' >/etc/sysctl.d/70-rpod-zram.conf

# MPD listens on a Unix socket only (system/mpd/mpd.conf), but Debian's unit
# starts it after network.target -- which NetworkManager reaches ~20 s into
# the boot, and rpod waits for MPD. The same unit minus that, in /etc; re-run
# this after an MPD upgrade to pick up Debian's changes to it.
sed -E '/^After=/s/ *\<network(-online)?\.target\>//' /usr/lib/systemd/system/mpd.service \
    >/etc/systemd/system/mpd.service

# NetworkManager after rPod's UI is up. Raspberry Pi OS patches it to run
# `netplan generate` (a Python script, then a daemon-reload) each time it
# loads its connections, several times at boot: ~10 s of the CPU and of
# systemd itself, all before it brings Wi-Fi up. Once rpod.service is ready
# -- rpod tells systemd after its first frame -- it's out of the UI's way.
mkdir -p /etc/systemd/system/NetworkManager.service.d
cat >/etc/systemd/system/NetworkManager.service.d/rpod.conf <<'EOF'
# system/boot/setup.sh
[Unit]
After=rpod.service
EOF

# The boot partition, mounted when something uses it (a kernel update, this
# script) and unmounted after a minute idle. Checking and mounting it at
# boot sat on the critical path for 1.2 s, and the music only needs the
# root filesystem; unmounted, a power cut can't leave it dirty either.
[ -e /etc/fstab.orig ] || cp /etc/fstab /etc/fstab.orig
awk '$2 == "/boot/firmware" && $4 !~ /x-systemd\.automount/ {
         $4 = $4 ",noauto,x-systemd.automount,x-systemd.idle-timeout=60"
     } { print }' /etc/fstab >/etc/fstab.rpod
mv /etc/fstab.rpod /etc/fstab
systemctl daemon-reload

# The kernel, uncompressed now and after every kernel update (rpod-kernel).
install -m 755 rpod-kernel /etc/kernel/postinst.d/z60-rpod-kernel
/etc/kernel/postinst.d/z60-rpod-kernel

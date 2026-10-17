#!/bin/sh
# The PiSugar 2 under the Pi (docs/PLAN.md §7.6): /dev/i2c-1 and i2c-tools,
# its power policy (rpod-pisugar: power kept on at light load while running,
# cut after a poweroff), its RTC keeping the clock, and logind letting rpod
# power off when the battery runs out. The RTC's driver comes from the boot
# config (dtoverlay=i2c-rtc,sd3078, system/config.txt.d).
#
# Runs on the Pi as root, from a copy of the repo's system/ directory --
# system/setup.sh runs it, and `make pisugar-setup` runs just this. Safe to
# re-run; harmless on a Pi without a PiSugar.
set -eu

cd "$(dirname "$0")"

apt-get install -y --no-install-recommends i2c-tools

install -m 644 rpod-i2c.conf /etc/modules-load.d/
modprobe i2c-dev

install -d /usr/local/lib/rpod
install -m 755 rpod-pisugar /usr/local/lib/rpod/
# systemd-shutdown runs everything here, with "poweroff", "halt", "reboot"
# or "kexec", once the filesystems are unmounted or read-only.
install -d /usr/lib/systemd/system-shutdown
ln -sfn /usr/local/lib/rpod/rpod-pisugar /usr/lib/systemd/system-shutdown/rpod-pisugar

install -m 644 rpod-pisugar.service rpod-rtc.service /etc/systemd/system/
install -m 644 99-rpod-rtc.rules /etc/udev/rules.d/
install -m 644 50-rpod-power.rules /etc/polkit-1/rules.d/
systemctl daemon-reload
systemctl enable rpod-pisugar.service
systemctl start rpod-pisugar.service
udevadm control --reload

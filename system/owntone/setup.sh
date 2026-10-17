#!/bin/sh
# AirPlay for rPod (docs/PLAN.md §6.3b): OwnTone, from its own apt archive
# for Raspberry Pi OS (Debian doesn't package it), playing MPD's "AirPlay
# feed" to the speaker Now Playing's "Play On" list picks.
#
# Runs on the Pi as root, from a copy of the repo's system/ directory --
# system/setup.sh runs it after bluetooth/setup.sh (which installs mpd.conf,
# feed and all), and `make owntone-setup` runs just this. Safe to re-run.
set -eu

cd "$(dirname "$0")"
export DEBIAN_FRONTEND=noninteractive

# OwnTone's archive, as its install docs give it
# (https://forums.raspberrypi.com/viewtopic.php?t=49928): owntone.asc is
# owntone/owntone-apt's repo/rpi/owntone.gpg, key
# E0AD 26FB 8252 5DC9 58B0  A1D4 F627 9331 9870 F4F5.
. /etc/os-release
install -m 644 owntone.asc /usr/share/keyrings/owntone-archive-keyring.asc
cat >/etc/apt/sources.list.d/owntone.sources <<EOF
Types: deb
URIs: http://www.gyfgafguf.dk/raspbian/owntone/
Suites: $VERSION_CODENAME
Components: contrib
Signed-By: /usr/share/keyrings/owntone-archive-keyring.asc
EOF

# The package runs OwnTone as owntone.conf's uid but makes no user for it.
getent passwd owntone >/dev/null ||
    useradd --system --user-group --home-dir /var/cache/owntone --no-create-home \
            --shell /usr/sbin/nologin owntone

apt-get update
# confold: an upgrade keeps rPod's /etc/owntone.conf.
apt-get install -y --no-install-recommends -o Dpkg::Options::=--force-confold owntone
# Its databases too: the package starts it once with whatever config is
# there, which may have made them as another user.
chown -R owntone:owntone /var/cache/owntone

install -m 644 owntone.conf /etc/owntone.conf
install -D -m 644 owntone.service.d/rpod.conf /etc/systemd/system/owntone.service.d/rpod.conf
install -m 644 rpod-owntone-watchdog.service rpod-owntone-watchdog.timer /etc/systemd/system/
systemctl daemon-reload
systemctl enable owntone.service
systemctl enable --now rpod-owntone-watchdog.timer
# A first start that failed (above) may have used up its restart limit.
systemctl reset-failed owntone.service 2>/dev/null || true
systemctl restart owntone.service

# Speakers show up a few seconds after it starts, as avahi finds them.
sleep 8
echo
echo "== AirPlay speakers OwnTone has found"
curl -s --max-time 5 http://127.0.0.1:3689/api/outputs |
    grep -o '"name": *"[^"]*"\|"type": *"[^"]*"' | paste - - || true

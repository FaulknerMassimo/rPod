<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/wordmark-on-dark.svg">
    <img src="docs/logo/wordmark-on-light.svg" alt="rPod" width="260">
  </picture>
</h1>

An iPod-style music player on a Raspberry Pi Zero 2 W, with an original
4th-gen iPod click wheel, a 2" 320×240 ST7789V LCD and a PiSugar 2
battery. MPD plays the library to Bluetooth headphones. A web UI uploads
music and videos and reaches the settings the wheel can't.

Full spec, hardware BOM, GPIO map, and phased build plan: [`docs/PLAN.md`](docs/PLAN.md).

## Status

The panel and the click wheel are wired to the Pi Zero 2 W that goes in
the iPod, running on a PiSugar 2, after bring-up on a Raspberry Pi 3B
breadboard dev board. Phases are in [`docs/PLAN.md`](docs/PLAN.md) §9.

- **Phase 0 (bring-up)**: done. `make pi-setup` sets up a fresh Raspberry
  Pi OS Lite install, `make build` cross-compiles on the PC against a
  sysroot copied off the Pi, and `make deploy-run` installs it.
- **Phase 1 (display)**: done and verified on hardware. The 2" ST7789V runs
  landscape 320×240 through fbtft, rotated in software so scrolling doesn't
  tear. LVGL animates at a measured 30 fps.
- **Phase 2 (click wheel)**: the bit map is derived on hardware
  ([`docs/clickwheel-protocol.md`](docs/clickwheel-protocol.md)), and the
  `rpod-wheel` daemon (`make deploy-wheel`) drives the UI on the Pi.
- **Phase 3 (audio)**: deferred. The Zero 2 W has no headphone jack, so
  audio is Bluetooth (Phase 6); the wired DAC may come later.
- **Phase 4 (UI)**: built and running on the Zero 2 W with the real wheel:
  the full §8.1 screen graph, scroll acceleration and alphabet scrub, Now
  Playing with cover art, a backlight timer and a sleep timer. Settings →
  Theme switches between rPod's own Glass look and Classic / Classic Dark,
  the 4th-gen iPod's UI rebuilt from its firmware (§8.4). Extras has a
  clock with an alarm, Brick, Music Quiz and Screen Lock (§8.5), and Home
  Assistant, which works your lights, locks and thermostat (§8.8). Still to
  do: the acceptance run, which means navigating a 1000+ track library to
  a song and playing it with only the wheel, and tuning scroll acceleration
  by feel. The desktop simulator (`make sim`) runs the same UI against a
  local MPD.
- **Phase 5 (power and USB)**: the battery is done, on a PiSugar 2 (§7.6).
  The USB gadget is deferred, since music goes on through the web UI. The
  status bar, About and the web UI show the charge, a notice comes up at
  20% and again at 10%, and rPod powers off before the battery runs flat.
  Settings → Shut Down powers off, and so does Auto-Off after a spell idle
  on battery. The PiSugar's clock keeps the time until Wi-Fi is up. Still
  to do: a discharge curve recorded on this cell (PiSugar's own doesn't
  fit it), telling charged from charging, and seeing the PiSugar cut the
  power after a power-off.
- **Phase 6 (Bluetooth)**: running on the Pi. Settings → Bluetooth pairs
  and connects headphones over BlueZ, and MPD plays to them through a
  system-wide PipeWire. Bluetooth audio is lossy, so the headphones get a
  re-encoded copy of whatever file is playing. AirPods get their extras
  over Apple's accessory protocol: battery, noise control, their settings,
  ear detection and Conversation Awareness. Verified with AirPods Pro 2 on
  the 3B dev board so far: the accessory channel, ear detection, and that
  the adapter is bondable.
  Still to verify: that a fresh pairing bonds and reconnects when the case
  opens, stem presses, and swipe volume.
- **Phase 7 (hardening)**: not started.

The **web UI** (`http://rpod.local/`, [`docs/PLAN.md`](docs/PLAN.md) §8.6)
uploads music, manages the library and reaches the settings the wheel
can't: the click wheel's acceleration curve, live; ListenBrainz; Home
Assistant's address and token; MPD's crossfade and Sound Check. It wears the device's theme, Glass or the
iPod's Classic, and follows it as it changes.

**Videos** (§8.7): upload one in the web UI and rPod converts it to fit
its screen, then plays it under Main Menu → Videos, the picture full
screen and the sound through MPD to the headphones or Bluetooth. Built and
tested in the simulator and headless; still to try on the Pi: conversion
speed, and lining the picture up with Bluetooth sound.

**Booting** ([`docs/PLAN.md`](docs/PLAN.md) §3): the rPod logo comes up
about 10 seconds after the power switch, while the Pi boots behind it, and
the menu at about 22. MPD's own start is most of what's left.

Haptics (Settings → Haptics) waits on the vibration motor being wired.

## Wiring

[![Pi Zero 2 W wired to the 2" display and the click wheel](docs/wiring-zero2w.svg)](docs/wiring-zero2w.svg)

The display and the click wheel on the Pi Zero 2 W's header (click the
picture for full size). Pin-by-pin tables, and the rest of the header's
pins, are in [`docs/wiring.md`](docs/wiring.md).

## Building

First time? [`docs/SETUP.md`](docs/SETUP.md) is the full guide: parts, wiring,
flashing the Pi, installing rPod, music, Bluetooth and AirPlay. The commands
below are its short form.

```sh
# The iPod's own fonts and bitmaps for the Classic themes, from Apple's
# firmware (once per checkout; see below)
make ipod-assets

# Desktop UI simulator (no hardware needed)
make sim

# Set up a fresh Raspberry Pi OS Lite 64-bit on rpod.local (user rpod, with
# passwordless sudo): packages, pigpio, boot config, MPD, Bluetooth, and
# everything that makes it boot fast. Once, then reboot the Pi.
make pi-setup

# Cross-compile for the Pi (host needs clang + lld). `make sysroot` copies the
# Pi's headers/libraries into ./sysroot first -- once, and again after
# installing new -dev packages on the Pi.
make sysroot
make build

# Deploy + run on hardware (rpod.local)
make deploy-run

# The web UI: on the Pi (then http://rpod.local/), or on the dev machine
# against the sim's MPD and the sim (then http://localhost:8080/)
make deploy-web
make web-sim
```

Uploads need the music directory writable by the `rpod` user
(`make deploy-web` says if it isn't). To ask for a password, put
`RPOD_WEB_PASSWORD=...` in `/etc/rpod/env`.

The Classic themes draw with the iPod's own fonts and bitmaps. They're
Apple's, so they aren't in the repository: `make ipod-assets`, once per
checkout, downloads the iPod 4G's firmware from Apple and extracts them into
`re/ipod4/out/` with `re/ipod4/tools/extract.py` (python3 only). Without
them, Classic keeps its layout but falls back to Montserrat, and the build
says so.

## Layout

- `src/` — on-device application (UI, input, MPD client, Bluetooth,
  battery, video).
- `daemon/` — `rpod-wheel`, the privileged click wheel decoder, and the
  `wheel_protocol.h`/`wheel_bits.h` headers shared with its clients; and
  `rpod-splash`, the logo on the screen while the Pi boots.
- `web/` — `rpod-web`, the web UI's daemon, and the page it serves
  (`web/static/`).
- `system/` — `setup.sh` (`make pi-setup`), boot config fragments, systemd
  units, udev rules, and MPD, Bluetooth, PiSugar and boot-time setup.
- `tools/` — `wheel-sniff` (protocol analysis), `wheel-test-client` (prints
  normalised wheel events), `fb-test` (raw framebuffer colour/orientation
  check), `fake-airpods.py` (AirPods stand-in for the sim), and `sim/`
  (desktop UI harness).
- `tests/` — host-run unit tests (`make test`).
- `docs/logo/` — the rPod wordmark, light and dark, and its square mark.
- `third_party/` — vendored LVGL.

## License

Copyright © 2026 Massimo Faulkner. rPod's code, docs and configuration are
licensed under [Creative Commons Attribution-NonCommercial 4.0
International](https://creativecommons.org/licenses/by-nc/4.0/) (CC BY-NC
4.0) — full text in [`LICENSE`](LICENSE).

You're free to use, copy, modify and share it, as long as:

- **you credit it** — name the author, link to this project and the
  license, and say if you changed anything. For example: "Based on rPod by
  Massimo Faulkner, licensed under CC BY-NC 4.0."
- **you don't use it commercially** — no selling it, or kits, boards or
  products built from it, without asking first.

Third-party parts keep their own licenses: LVGL (`third_party/lvgl`) is MIT,
and the Montserrat fonts in `src/ui/fonts/` and `web/static/fonts/` are SIL
OFL 1.1 (`web/static/fonts/Montserrat-OFL.txt`). The AirPods
pictures in `src/ui/airpods_art_data.c` come from Wikimedia Commons — the
buds from a photo by Burkhard Mücke (CC BY-SA 4.0, which that image keeps),
the case from one by Jackienyyy (CC0) — and the AirPods glyph is
MingCute's (Apache-2.0); see `tools/airpods-art.py`. Click wheel
decoding derives from `dupontgu/retro-ipod-spotify-client` (Apache-2.0) —
see `docs/PLAN.md` §12. AirPods support is reimplemented from the protocol
notes in LibrePods (`kavishdevar/librepods`, GPL-3.0); none of its code is
included.

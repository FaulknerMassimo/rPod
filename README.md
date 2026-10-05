# rPod

A Raspberry Pi Zero 2 W music player built around an original 4th-gen iPod
click wheel, a 2" ST7789V LCD, and an I²S DAC for bit-perfect lossless
playback via MPD.

Full spec, hardware BOM, GPIO map, and phased build plan: [`docs/PLAN.md`](docs/PLAN.md).

## Status

Phase 1 (display) — done and hardware-verified: landscape 320×240 via
fbtft's `rotate=90`, LVGL rendering and animating at a measured 30 fps.

Phase 2 (click wheel) — `daemon/rpod-wheel.c` and `tools/wheel-test-client`
are written and build clean, but the click wheel's real bit map has not been
derived on hardware yet: `daemon/wheel_bits.h` intentionally fails to build
until `tools/wheel-sniff.c` has been run on the actual wheel and
`docs/clickwheel-protocol.md` filled in (`docs/PLAN.md` §4.3). The physical
wheel is currently dead, so this step is blocked until a replacement is
sourced.

Phase 3 (audio) — not started; the DAC isn't wired up yet.

Phase 6 (Bluetooth) — written but **not yet run on hardware**:
`make bluetooth-setup` installs BlueZ plus a system-wide PipeWire/WirePlumber
that MPD's `Bluetooth` output plays into (`docs/PLAN.md` §6.3). Settings →
Bluetooth turns the adapter on/off, searches for headphones/speakers, and
pairs/connects/forgets them over BlueZ's D-Bus API. It's tested headless
against a mock BlueZ only, so far. AirPods get their extras over Apple's
accessory protocol (`src/audio/airpods.c`): battery, noise control, their
settings, pause on ear removal, Conversation Awareness ducking and stem
presses. That's tested against `tools/fake-airpods.py` only, not real
AirPods yet.

UI (pulled forward from Phase 4, ahead of hardware) — the full §8.1 screen
graph (Main Menu, Music browse/playback, Now Playing, Settings, Extras)
is built and runs in the desktop simulator against a real local MPD
instance (`make mpd-dev`, then `make sim`). Since there's no working click
wheel to test against, `tools/sim/sim_input.c` stands in with the keyboard:
Left/Right arrows rotate, Enter selects, M/Space/N/P are Menu/
Play-Pause/Next/Prev (Menu is M, not Escape — see `tools/sim/sim_input.c`
for why Escape collides with the encoder's own key handling).
`src/main.c` (the on-device binary) is untouched —
wiring the real wheel socket into these same screens has to wait for
working wheel hardware. See `docs/PLAN.md` §9 for the phase list and
acceptance criteria.

## Building

```sh
# Desktop UI simulator (no hardware needed)
make sim

# Cross-compile for the Pi (host needs clang + lld). `make sysroot` copies the
# Pi's headers/libraries into ./sysroot first -- once, and again after
# installing new -dev packages on the Pi.
make sysroot
make build

# Deploy + run on hardware (rpod.local)
make deploy-run
```

## Layout

- `src/` — on-device application (UI, audio client, library index, power).
- `daemon/` — `rpod-wheel`, the privileged click wheel decoder, and the
  `wheel_protocol.h`/`wheel_bits.h` headers shared with its clients.
- `system/` — boot config fragments, systemd units, udev rules, USB gadget setup.
- `tools/` — `wheel-sniff` (protocol analysis), `wheel-test-client` (prints
  normalised wheel events), `fb-test` (raw framebuffer colour/orientation
  check), `fake-airpods.py` (AirPods stand-in for the sim), and `sim/`
  (desktop UI harness).
- `tests/` — host-run unit tests (`make test`).
- `third_party/` — vendored LVGL.

## License

Click wheel decoding derives from `dupontgu/retro-ipod-spotify-client`
(Apache-2.0) — see `docs/PLAN.md` §12. LVGL is MIT. AirPods support is
reimplemented from the protocol notes in LibrePods
(`kavishdevar/librepods`, GPL-3.0); none of its code is included.

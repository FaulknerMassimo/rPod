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
`docs/clickwheel-protocol.md` filled in (`docs/PLAN.md` §4.3). See
`docs/PLAN.md` §9 for the phase list and acceptance criteria.

## Building

```sh
# Desktop UI simulator (no hardware needed)
make sim

# Cross-compile for the Pi
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
  check), and `sim/` (desktop UI harness).
- `third_party/` — vendored LVGL.

## License

Click wheel decoding derives from `dupontgu/retro-ipod-spotify-client`
(Apache-2.0) — see `docs/PLAN.md` §12. LVGL is MIT.

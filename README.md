# rPod

A Raspberry Pi Zero 2 W music player built around an original 4th-gen iPod
click wheel, a 2" ST7789V LCD, and an I²S DAC for bit-perfect lossless
playback via MPD.

Full spec, hardware BOM, GPIO map, and phased build plan: [`docs/PLAN.md`](docs/PLAN.md).

## Status

Phase 0 (bring-up) — repository scaffold in progress. See `docs/PLAN.md` §9
for the phase list and acceptance criteria.

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
- `daemon/` — `rpod-wheel`, the privileged click wheel decoder.
- `system/` — boot config fragments, systemd units, udev rules, USB gadget setup.
- `tools/` — `wheel-sniff` (protocol analysis) and `sim/` (desktop UI harness).
- `third_party/` — vendored LVGL.

## License

Click wheel decoding derives from `dupontgu/retro-ipod-spotify-client`
(Apache-2.0) — see `docs/PLAN.md` §12. LVGL is MIT.

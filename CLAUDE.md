# rPod

This is a Raspberry Pi Zero 2 W iPod-style music player. Full spec, hardware
BOM, GPIO map, click wheel protocol, phased build plan, and risk register live
in `docs/PLAN.md` — read it before making changes.

## Ground rules

- Target is a Pi Zero 2 W: 512 MB RAM is the binding constraint, not CPU.
- No X11/Wayland. UI renders directly to DRM (`panel-mipi-dbi`, preferred) or
  fbdev (`fbtft`, fallback). See `docs/PLAN.md` §5.
- C is the primary language for the UI and input daemon. Python is for build
  tooling / one-off scripts only.
- Do not port the buggy `setBit()` bit-packing from the reference click wheel
  driver — see `docs/PLAN.md` §4.3. Derive real bit positions with
  `tools/wheel-sniff.c` on actual hardware.
- Follow the phased build plan (`docs/PLAN.md` §9) in order. Each phase has an
  acceptance test; don't skip ahead.
- The Pi is reachable at `rpod.local` once on the network. `make deploy` /
  `make deploy-run` rsync + restart the systemd unit over SSH.
- The desktop simulator (`tools/sim/`, LVGL SDL backend) is the fast iteration
  loop for UI work — build and test there before touching hardware.
- The screen is landscape (320×240), not portrait — click-wheel iPods (4th
  gen, Photo, Video/Classic, Mini) all had landscape screens sitting above
  the wheel, despite the device body being portrait overall. See
  `docs/PLAN.md` §5.

## Hardware debugging notes (fbtft / ST7789V panel)

Hard-won on real hardware — see `docs/PLAN.md` §5.3 for full detail:

- `LV_MEM_SIZE` needs real headroom above the fbdev partial-render draw
  buffers, or `LV_USE_ASSERT_MALLOC`'s handler turns a failed allocation
  into a silent infinite-loop hang: 100% CPU, no crash, no log output,
  screen just never updates. 512 MB total RAM makes generous headroom free.
- `LV_LINUX_FBDEV_MMAP` must stay `0` (pwrite) with `rotate=` active in the
  fbtft overlay — mmap'd writes from LVGL's long-running process silently
  never reach this panel, even though the same mechanism works fine from a
  short-lived test program. Don't flip it back to `1` without re-verifying
  on the actual panel, not just fps/CPU numbers — a wedged flush still
  leaves the process looking "active."
- This staging driver's internal state can wedge under heavy rapid testing
  (many opens/mmaps/writes across processes, no reboot in between): writes
  stop reaching the panel with zero kernel-side error. Reboot the Pi and
  re-test before concluding a config change broke something — don't assume
  it's a wiring problem either without ruling this out first.

## Repo layout

See `docs/PLAN.md` §2 for the full annotated tree.

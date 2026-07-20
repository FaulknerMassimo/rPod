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

## Repo layout

See `docs/PLAN.md` §2 for the full annotated tree.

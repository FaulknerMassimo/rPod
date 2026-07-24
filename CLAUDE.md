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
- The default `classic` board's screen is landscape (320×240), not portrait —
  click-wheel iPods (4th gen, Photo, Video/Classic, Mini) all had landscape
  screens sitting above the wheel, despite the device body being portrait
  overall. See `docs/PLAN.md` §5. (rPod is multi-board now — the Waveshare
  1.44" LCD HAT is a 128×128 *square* panel; see the multi-board rule below.)
- The physical click wheel is currently fried/dead and the DAC isn't wired
  up, so Phase 2's and Phase 3's hardware bring-up steps (`docs/PLAN.md`
  §9) are blocked until replacement hardware shows up. Don't propose
  wheel-sniffing or DAC bring-up work in the meantime — UI work happens in
  the simulator (`tools/sim/`) against a real local MPD instance instead
  (`make mpd-dev-conf && make mpd-dev`, then `make sim`), with the keyboard
  standing in for the wheel (`tools/sim/sim_input.c`: Left/Right rotate,
  Enter selects, M/Space/N/P are Menu/Play-Pause/Next/Prev).
- rPod supports multiple boards, chosen at runtime by the `RPOD_BOARD` env
  var (`src/platform/board.h`, `docs/PLAN.md` §5.5). Screens read runtime
  metrics (`src/ui/metrics.h` — `rpod_metrics()`), not compile-time size/font
  constants; keep the **landscape profile byte-identical** when touching them
  (verify with a headless frame-dump diff). The **Waveshare 1.44" LCD HAT**
  (`RPOD_BOARD=waveshare-144`: 128×128 ST7735S + 5-way joystick / KEY1-3 read
  via libgpiod, `src/input/gpio_buttons.c`) is the first board whose display
  *and* buttons both work on real hardware, so it runs the full on-device UI
  (`src/main.c` → the shared `src/app.c`), not the Phase-1 stub. Iterate on its
  128×128 layout with `RPOD_BOARD=hat144 make sim`.

## Hardware debugging notes (fbtft / ST7789V panel)

Hard-won on real hardware — see `docs/PLAN.md` §5.3 for full detail:

- `LV_MEM_SIZE` needs real headroom above the fbdev partial-render draw
  buffers, or `LV_USE_ASSERT_MALLOC`'s handler turns a failed allocation
  into a silent infinite-loop hang: 100% CPU, no crash, no log output,
  screen just never updates. 512 MB total RAM makes generous headroom free.
  256 KB (enough for the Phase 1 label+spinner scene) was not enough once
  real content showed up in Phase 4: a 552-row flat song list segfaulted
  inside `lv_obj_class_create_obj()` growing a widget's children array —
  that particular allocation-failure path isn't guarded by
  `LV_USE_ASSERT_MALLOC`, so it's a hard crash there rather than the hang
  above. Bumped to 4 MB in both `src/ui/lv_conf.h` and `tools/sim/lv_conf.h`
  (kept in sync — see the comment in either). Since `LV_MEM_SIZE` is a
  compile-time macro baked into every LVGL translation unit, a plain
  incremental `make` after changing it can link stale objects — `make
  clean` first.
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

## UI / MPD simulator notes

Hard-won building the Phase 4 screen graph (`src/ui/screens/`) against a
real local MPD instance in `tools/sim/` — see `src/audio/mpd_client.c` and
`tools/sim/sim_input.c` for the code these apply to:

- libmpdclient: `mpd_search_add_db_songs()` is the *searchadd* variant — it
  queues matches onto the server-side play queue as a side effect instead
  of just listing them. Use `mpd_search_db_songs()` (no `_add_`) for a pure
  listing query, read via `mpd_recv_song()`. Caught by testing against a
  real server: browsing an artist's album was silently enqueuing its songs.
- libmpdclient latches an error on the connection after any failed
  command; until `mpd_connection_clear_error()` is called, every
  subsequent command silently fails too — one unsupported query (e.g.
  `listplaylists` with no `playlist_directory` configured) permanently
  wedges the connection for the rest of the session otherwise. Every
  failure path in `mpd_client.c` clears it (see the `fail()` helper there).
- MPD's `search`/`find` commands reject a query with zero constraints
  (`ACK ... too few arguments for "search"`) — unlike `list <tag>`, which
  is happy to enumerate everything unfiltered. The flat, unfiltered "Songs"
  browse screen has to use the recursive database listing
  (`mpd_send_list_all_meta` / `listallinfo`) instead of a constraint-less
  search.
- The vendored LVGL's SDL keyboard driver
  (`third_party/lvgl/src/drivers/sdl/lv_sdl_keyboard.c`) has a real bug:
  its simulated key-release read sets `state = RELEASED` but never sets
  `key`, leaving it uninitialized — and `indev_encoder_proc()` checks that
  field on release to decide whether to fire a select. In this build it
  consistently read back as `LV_KEY_ENTER`, so *every* key release
  (including plain rotation) fired a spurious select on whatever had just
  been rotated onto. Don't build the wheel-simulator indev on
  `lv_sdl_keyboard_create()` + a type override; `sim_input.c` instead polls
  `SDL_GetKeyboardState()` directly and drives a custom encoder read
  callback that always sets both `state` and `key` explicitly.
- To debug LVGL input/navigation bugs, don't reach for a real display or
  screenshots — a headless `lv_display_create()` with a no-op flush
  callback plus either direct `lv_obj_send_event()` calls or a scripted
  indev read callback reproduces push/pop/click bugs deterministically and
  runs under gdb/ASan with no window at all. This is also the *only* safe
  way to inspect the running sim's behavior from here: an `ffmpeg
  x11grab`/`spectacle` screenshot of the real display captured the user's
  actual desktop (browser tabs and all), not just the sim window — never
  do that for verification.

## Repo layout

See `docs/PLAN.md` §2 for the full annotated tree.

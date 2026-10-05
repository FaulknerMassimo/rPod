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
- The Pi at `rpod.local` is currently a **Raspberry Pi 3B breadboard dev
  board** (Debian trixie, user `rpod`, passwordless sudo), not the Zero 2 W —
  it's where the real hardware's wiring gets brought up (`docs/PLAN.md`
  §1.1). Its boot config is `system/config.txt.d/rpod.txt` minus the two
  lines that fragment's header calls out, plus `fbcon=map:9` in
  `cmdline.txt`. MPD (`system/mpd/mpd.conf`, Unix socket only) plays a
  sample library in `/media/music` through the 3B's headphone jack.
- Build on the dev machine, not the Pi: `make build` cross-compiles with
  clang + lld against `./sysroot`, a copy of the Pi's own headers/libraries
  (`make sysroot`; re-run after installing new `-dev` packages on the Pi).
  A full build takes seconds here vs. many minutes on the Pi.
- The desktop simulator (`tools/sim/`, LVGL SDL backend) is the fast iteration
  loop for UI work — build and test there before touching hardware.
- The screen is landscape (320×240), not portrait — click-wheel iPods (4th
  gen, Photo, Video/Classic, Mini) all had landscape screens sitting above
  the wheel, despite the device body being portrait overall. See
  `docs/PLAN.md` §5.
- A replacement click wheel is wired to the dev board (CLOCK GPIO 23, DATA
  GPIO 25). Its bit map is derived (`docs/clickwheel-protocol.md`), and
  `make deploy-wheel` installs the `rpod-wheel` daemon. The daemon and
  sniffer need pigpio, built from source on the Pi into `/usr/local` (`make
  sysroot` copies that too). The Pi's `/tmp` is tmpfs, so copy one-off
  tools to the `rpod` home directory, not `/tmp`. The
  DAC still isn't wired, so Phase 3's hardware bring-up stays blocked —
  don't propose DAC work. UI work still happens in the simulator
  (`tools/sim/`) against a real local MPD instance (`make mpd-dev-conf &&
  make mpd-dev`, then `make sim`), with the keyboard standing in for the
  wheel (`tools/sim/sim_input.c`: Left/Right rotate, Shift+Left/Right
  jumps a letter like a fast flick, Enter selects, M/Space/N/P are
  Menu/Play-Pause/Next/Prev -- held keys are held buttons, so Space held
  sleeps and N/P held seek), or the real wheel through an SSH-forwarded
  daemon socket (`RPOD_WHEEL_SOCK`, see `tools/sim/sim_main.c`).

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
- The firmware silently truncates `config.txt` lines at 98 characters. The
  fbtft overlay line used to run past it and silently lost `speed=` (panel
  stuck at 32 MHz). Keep overlay params split across `dtparam=` lines (see
  `system/config.txt.d/rpod.txt`), and check `sudo vclog -m` for
  `Unknown dtparam` after touching boot config.
- fbtft pushes a *full* frame over SPI on any `write()`, so measure display
  work with the SPI counters (`/sys/bus/spi/devices/spi0.0/statistics`
  `bytes_tx`), not fps/CPU. mmap was re-tested this way on kernel 6.18:
  page-granular pushes worked for a while after boot, then stopped entirely
  (even for fresh mappings) until reboot, while `pwrite()` kept working.
  `src/ui/lvgl_port.c` writes each LVGL refresh in one `pwrite()`.
- Never blank the panel with `FBIOBLANK`: fbtft writes DISPOFF/DISPON from
  the ioctl while its deferred-I/O worker may be mid-frame on the same SPI
  bus and D/C line, unsynchronised. Seen in dmesg (fb0's `debug` sysfs
  attribute, bit 21 = register writes): DISPOFF ~20 ms into a RAMWR stream,
  and sleep/wake cycles left the panel black with the backlight on while
  frames kept flowing. Sleep switches only the backlight, via
  `/sys/class/backlight/fb_st7789v/bl_power` (GPIO only, no SPI; udev rule
  in `system/udev/99-rpod-panel.rules` makes it writable by `video`).
- A wheel that goes silent (daemon up, UI connected, `wheel-test-client`
  shows nothing) can be pigpio's DMA sampler stalled, not wiring: its
  control blocks live in mlock'd pages (`PI_MEM_ALLOC_PAGEMAP`), and memory
  compaction under page-cache pressure (a big rsync) migrated them -- DMA
  channel 14 then shows garbage src/dst and `DREQ_STOPS_DMA`.
  `rpod-wheel.service` now sets `vm.compact_unevictable_allowed=0` first;
  restarting the daemon recovers a stalled one.
- The panel can also go black with frames still flowing (SPI `bytes_tx`
  rising, `/dev/fb*` holding the right image). Re-binding the driver
  (`systemctl stop rpod`, then `spi0.0` to
  `/sys/bus/spi/drivers/fb_st7789v/unbind` and `bind`) re-runs its init
  without a reboot. It may come back as `fb0` instead of `fb1`; rpod
  follows it through the `/dev/rpod-panel` udev symlink.
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
- Cover art is expensive on the Pi: real rips embed 1-4 MB 1400x1400 PNGs,
  ~150 ms to decode (most of it zlib), and fetching one at MPD's default 8
  KiB `binarylimit` took 3-4 s (`rpod_mpd_connect()` now negotiates 1 MiB).
  Worse, MPD serves `readpicture` on its main thread, re-reading the file
  for every chunk: while any connection fetches a cover, *every* client
  waits -- the UI's status poll (0.2 ms normally) stalled up to 1.4 s. So
  covers are read straight out of the FLAC (`src/audio/embedded_art.c`,
  using MPD's `config` music_directory); MPD is only the fallback for
  non-FLAC files. Never fetch/decode covers on the LVGL thread -- everything,
  Now Playing included, goes through `src/ui/cover_cache.h`: worker threads,
  a disk tier of one decoded tile per album (`/var/cache/rpod/covers`, sim:
  `~/.cache/rpod-sim/covers`), and a niced background pass that fills it for
  the whole library whenever MPD's database changes. Bump its
  `DISK_VERSION` when the decoder's output changes; clear the directory
  after re-tagging art (it's never revalidated).
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
- Settings → Bluetooth (`src/audio/bluetooth.c`) uses the *system* D-Bus,
  so in the sim it drives the dev machine's own BlueZ: toggling power or
  pairing there acts on the desktop's real adapter. Test it against
  python-dbusmock's `bluez5` template on a private bus instead
  (`DBUS_SYSTEM_BUS_ADDRESS`, which sd-bus honors), never the real one.
- BlueZ only bonds while some agent is registered. With none, and
  `AlwaysPairable = false` (its default), the adapter is non-bondable and
  the kernel pairs with "no bonding": `bluetoothctl info` says `Paired:
  yes` but `Bonded: no`, the link key is gone at disconnect, and headphones
  need pairing mode every time. `src/audio/bluetooth.c` registers a
  NoInputNoOutput agent and claims the default one on the device (`sudo
  btmgmt info` should list `bondable`). The sim registers it without
  claiming the default, which would hijack the desktop's own pairing
  prompts.
- AirPods extras (`src/audio/airpods.c`) follow BlueZ too, so in the sim
  they'd open Apple's accessory channel (L2CAP PSM 0x1001) to the desktop's
  own AirPods if they're connected, and claim their stem presses for the
  sim's MPD. Run the sim with `RPOD_AIRPODS_SOCK` pointed at
  `tools/fake-airpods.py` instead, which takes BlueZ out of it entirely
  (commands on its stdin play the AirPods' side: `out`, `talk 1`, `press
  double`...). `make test` covers the packet codec against real captures.
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

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
- The Pi at `rpod.local` is the **Raspberry Pi Zero 2 W** that goes in the
  iPod (Raspberry Pi OS Lite 64-bit, Debian trixie, user `rpod`,
  passwordless sudo), with the display and the click wheel wired to it. A
  Pi 3B breadboard dev board brought that wiring up first (`docs/PLAN.md`
  §1.1). `make pi-setup` (`system/setup.sh`) sets up a fresh install:
  packages, pigpio, the boot config (`system/config.txt.d/rpod.txt`, plus
  `fbcon=map:9 quiet logo.nologo` and two boot-time flags in
  `cmdline.txt`), boot time (`system/boot/`), MPD
  (`system/mpd/mpd.conf`, Unix socket only; the Zero 2 W has no headphone
  jack), Bluetooth audio and AirPlay (OwnTone, `system/owntone/`, playing
  MPD's "AirPlay feed"; `docs/PLAN.md` §6.3b). Then reboot, `make
  sysroot`, and `make deploy-run deploy-wheel deploy-web`. The library is in
  `/media/music`. mDNS for `rpod.local` is flaky on this network: when it
  won't resolve, use the Pi's IP with `SSH="ssh -o HostKeyAlias=rpod.local"`
  (the Makefile's header).
- A PiSugar 2 under the Zero 2 W is its battery (`docs/PLAN.md` §7.6):
  `src/power/battery.c` reads its IP5209 power chip (I²C 0x75) for the
  status bar, About and the web UI, and powers off when it runs out.
  Settings > Shut Down and Auto-Off (`src/ui/shutdown.c`: idle, on
  battery, no alarm set) power off for real too; the web UI holds Auto-Off
  off while it's in use (`active` on the settings socket).
  `system/pisugar/` (`make pisugar-setup`, part of `pi-setup`) stops the
  IP5209 switching off at light load while the system's up, has it cut the
  power after a poweroff (a systemd-shutdown hook), and lets rpod power off
  through logind (polkit). Its SD3078 RTC keeps the clock (`i2c-rtc`
  overlay). The sim has no battery unless `RPOD_BATTERY_FAKE` fakes one
  (`15`, `80,charging`, `100,charged`, `2,critical`), and never powers
  anything off.
- Boot time (`docs/PLAN.md` §3): the logo is up ~10 s after power-on,
  the menu ~22 s. `rpod-splash.service` (`daemon/rpod-splash.c`) loads the
  panel's driver itself and draws the logo the moment fbtft's framebuffer
  appears; rpod.service is ordered after it. rpod.service is
  `Type=notify`: rpod reports ready after its first frame (`src/app.c`),
  and NetworkManager is ordered after it, so any new startup path in rpod
  must still get there, or systemd kills it after 30 s. `make boot-setup`
  (`system/boot/setup.sh`) holds the rest: cloud-init and rpi-swap's
  generator masked (zram from zram-generator), the kernel kept
  uncompressed by a kernel hook, `/boot/firmware` an automount, and an
  `/etc` copy of mpd.service without network.target (re-run after an MPD
  upgrade). Try boot config changes through tryboot (`tryboot.txt`, then
  `sudo reboot "0 tryboot"`): one boot reads it, and a power cycle goes
  back to `config.txt`, so a bad change can't strand the Pi.
- Build on the dev machine, not the Pi: `make build` cross-compiles with
  clang + lld against `./sysroot`, a copy of the Pi's own headers/libraries
  (`make sysroot`; re-run after installing new `-dev` packages on the Pi).
  A full build takes seconds here vs. many minutes on the Pi.
- The desktop simulator (`tools/sim/`, LVGL SDL backend) is the fast iteration
  loop for UI work — build and test there before touching hardware.
- The web UI (`docs/PLAN.md` §8.6, `web/`) wears the device's theme, so
  a look change on the device has a web counterpart in
  `web/static/app.css` (same colours as `rpod_theme()`).
- Themes (`docs/PLAN.md` §8.4): Glass, plus Classic / Classic Dark, the
  4th-gen iPod's UI recovered in `re/ipod4/` (`SPEC.md`, and the extractor
  in `tools/`). The iPod's fonts and bitmaps are Apple copyright, and this
  repo is public -- never commit the firmware, `re/ipod4/out/` or any
  generated `ipod_font_*`/`ipod_images*` source. `make ipod-assets`
  downloads the firmware from Apple and generates them; the Makefile
  compiles them from `IPOD_ASSETS` when present, and both builds must still
  work without them. Colours come from `rpod_theme()` (the `RPOD_COLOR_*` macros), layout
  numbers from `rpod_metrics()`; text on a selection is
  `RPOD_COLOR_SEL_TEXT`, not `RPOD_COLOR_TEXT` (in Classic both the
  selection and the text are black). Glass must stay pixel-identical when
  touching Classic -- diff headless screenshots against the previous build.
- The screen is landscape (320×240), not portrait — click-wheel iPods (4th
  gen, Photo, Video/Classic, Mini) all had landscape screens sitting above
  the wheel, despite the device body being portrait overall. See
  `docs/PLAN.md` §5.
- A replacement click wheel is wired to the Pi (CLOCK GPIO 23, DATA
  GPIO 25). Its bit map is derived (`docs/clickwheel-protocol.md`), and
  `make deploy-wheel` installs the `rpod-wheel` daemon. The daemon and
  sniffer need pigpio, built from source on the Pi into `/usr/local` (`make
  sysroot` copies that too). The Pi's `/tmp` is tmpfs, so copy one-off
  tools to the `rpod` home directory, not `/tmp`. The
  DAC and the USB gadget are deferred for good reasons (audio is Bluetooth,
  uploads go through the web UI) -- don't propose DAC, USB gadget or Phase 5
  work. UI work still happens in the simulator
  (`tools/sim/`) against a real local MPD instance (`make mpd-dev-conf &&
  make mpd-dev`, then `make sim`), with the keyboard standing in for the
  wheel (`tools/sim/sim_input.c`: Left/Right rotate, Shift+Left/Right
  jumps a letter like a fast flick, Enter selects, M/Space/N/P are
  Menu/Play-Pause/Next/Prev -- held keys are held buttons, so Space held
  sleeps and N/P held seek; in Brick, held arrows keep turning), or the
  real wheel through an SSH-forwarded daemon socket (`RPOD_WHEEL_SOCK`,
  see `tools/sim/sim_main.c`). The
  backlight timer blacks out the sim window after 30 s idle, as on the
  device; any key wakes it (Settings → Backlight → Always On stops it).

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
- Write the panel's framebuffer with `pwrite()`, never through an mmap —
  mmap'd writes from LVGL's long-running process silently never reach this
  panel, even though the same mechanism works fine from a short-lived test
  program. Don't switch without re-verifying on the actual panel, not just
  fps/CPU numbers — a wedged flush still leaves the process looking
  "active."
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
  `src/ui/lvgl_port.c` writes each LVGL refresh in one `pwrite()`, then
  `fsync()`s it, which pushes at once and returns when the push is done
  (25.4 ms) — a quick way to time pushes from userspace too.
- Scrolling tore diagonally on every frame with fbtft's `rotate=90`: MADCTL
  rotation makes pushes sweep the glass across the panel's own row-by-row
  refresh. So fbtft runs at `rotate=0` and `lvgl_port.c` rotates in
  software; pushes then follow the refresh and only rarely tear, and in a
  straight line. That rarity relies on two timing fixes in
  `system/config.txt.d/rpod.txt` (`docs/PLAN.md` §5.3):
  `system/overlays/rpod-panel.dts` slows the panel's refresh (FRCTRL2) to
  roughly the push's speed, and `core_freq_min=400` stops the core clock
  idling at 275 MHz, which drags the SPI clock (divider fixed at boot) from
  50 to 34 MHz. Re-match FRCTRL2 if the SPI clock ever changes. If a straight
  vertical tear shows on most frames, try `rotate=180` (same picture, pushed
  the other way).
- `lvgl_port.c` installs LVGL's tick (`lv_tick_set_cb`). LVGL's fbdev driver
  used to do that as a side effect of `lv_linux_fbdev_create()`; replacing it
  dropped the tick, and with `lv_tick_get()` stuck at 0 no `lv_timer` fires:
  the UI still drew its first frame, but the click wheel never connected
  (no "click wheel connected" in `journalctl -u rpod`) and nothing updated.
  Any new display backend needs a tick source.
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
- The kernel sets the clock from the PiSugar's RTC as its module registers
  (`/sys/class/rtc/rtc0/hctosys` is 1), so an RTC that was never set or
  lost its time takes the clock back to 2002 until Wi-Fi syncs.
  `rpod-rtc.service` puts it forward to timesyncd's last saved time then,
  and the kernel writes the RTC back as soon as timesyncd syncs.
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
- A focusable widget whose Center handler can delete it (a spinner that
  pops its screen when done) must act on `LV_EVENT_CLICKED`, not
  `LV_EVENT_SHORT_CLICKED`: LVGL follows SHORT_CLICKED with
  SINGLE/DOUBLE_CLICKED to the same object, and an encoder indev isn't reset
  when its focused object goes, so that's a use-after-free -- two Center
  presses within the long-press time segfaulted in DOUBLE_CLICKED. CLICKED
  is the release's last event; the list rows already act on it.
- Music Quiz and the alarm change MPD's queue (the quiz puts it back, the
  alarm replaces it). Test them against a private MPD -- `audio_output`
  type `null`, its own socket and state files, the same `music_directory`
  -- rather than the sim's dev MPD while someone's listening to it.
- To debug LVGL input/navigation bugs, don't reach for a real display or
  screenshots — a headless `lv_display_create()` with a no-op flush
  callback plus either direct `lv_obj_send_event()` calls or a scripted
  indev read callback reproduces push/pop/click bugs deterministically and
  runs under gdb/ASan with no window at all. This is also the *only* safe
  way to inspect the running sim's behavior from here: an `ffmpeg
  x11grab`/`spectacle` screenshot of the real display captured the user's
  actual desktop (browser tabs and all), not just the sim window — never
  do that for verification.

## Web UI notes (rpod-web)

`web/` is the web UI (`docs/PLAN.md` §8.6): `rpod-web`, its own process,
serving `web/static/` and a JSON API. `make web-sim` runs it against the
sim's MPD and the sim (`http://localhost:8080/`); `make deploy-web`
installs it on the Pi. Things that matter when changing it:

- Settings rPod owns (theme, backlight, sleep, alarm, wheel tuning,
  ListenBrainz token) go through rPod's settings socket
  (`src/ui/control.c`), never by writing `/var/lib/rpod` files from
  rpod-web: rPod only reads those at startup, and it's their one writer.
  A new setting is a key in `control.c` plus a row in `web/static/app.js`.
- The API's protection is the Host check, the `X-Rpod: 1` header on
  anything that changes something, and the optional password
  (`web/api.c`'s header). Never add CORS headers or an endpoint that acts
  on a GET; keep library paths going through `rpod_lib_path_ok()` and the
  `openat(O_NOFOLLOW)` walk in `web/library.c` (`make test` covers both,
  symlink escapes included).
- Uploads stream to disk in pieces, pushed out and dropped from the page
  cache every 8 MB -- don't buffer a file in memory, and keep the cache
  pressure down (it once stalled the wheel's DMA, above).
- Classic's Chicago, Pixo and icons on the page come from `make
  ipod-assets` (`re/ipod4/out/web/`, served at `/ipod/`): Apple's, never
  committed, and the page must keep working without them. Glass's
  Montserrat in `web/static/fonts/` is OFL and is committed.
- A Unix socket path is at most 107 bytes: the scratchpad's paths are too
  long for one, so point `RPOD_CONTROL_SOCK` at a relative path (and run
  both processes from the same directory) when testing there.
- To test it end to end without touching the desktop: a private MPD on a
  scratch library, a headless rPod (an in-memory `lv_display`, like the
  input-debugging note above) with its own state paths, rpod-web in front,
  and headless Chromium driven over the DevTools protocol for screenshots
  and console errors -- never a screenshot of the real display.

## Home Assistant notes

Extras > Home Assistant (`docs/PLAN.md` §8.8, `src/ha/`,
`src/ui/screens/ha_screens.c`) is a REST client for a Home Assistant server.
Things that matter when changing it:

- Every request runs on `ha.c`'s worker thread; never call curl from the
  LVGL thread (a dead server takes 5-20 s to time out). Screens read
  snapshots (`rpod_ha_list()` returns a copy) and a watcher polls
  `rpod_ha_version()`; a live list rebuilds only when its signature
  changed (`ha_screens.c`), so don't bump the version for nothing.
- The token is a credential: `/var/lib/rpod/homeassistant` is 0600, the
  settings socket's `get` never returns it, never log it, and redirects stay
  off so it only goes to the configured address. The server's address and
  token come only from the web UI (`ha_url`, `ha_token` in `control.c`):
  rpod-web doesn't talk to Home Assistant itself.
- A list row's key is 63 bytes, so entity ids are capped at 63
  (`rpod_ha_valid_id()`); ids also go into a URL and a JSON body without
  escaping, which that check is what makes safe.
- Test against `tools/fake-ha.py` (port 8123, token
  `rpod-test-token-0123456789`; the sim's state file is
  `~/.local/state/rpod-sim/homeassistant`: the address, then the token, one
  per line), never someone's real Home Assistant -- an action there
  unlocks a real door. A headless `lv_display` drives the screens, like the
  input-debugging note above says.

## Video notes

Videos (`docs/PLAN.md` §8.7): `web/rpod_convert.c` makes every upload into
one plain kind, `src/video/` decodes the picture, MPD plays the sound and
its position is the clock. Things that matter when changing it:

- The videos folder (`RPOD_VIDEO_DIR`, `/media/videos`) must stay outside
  MPD's `music_directory`. MPD maps any path inside it back to its database,
  so an `.mpdignore`d video can't be played at all; outside it, MPD plays
  one by absolute path, but only for clients on its local socket.
- The Pi has FFmpeg 7.1 (libavcodec 61), the dev machine a newer one (9.x
  when this was written): use only APIs both have (`coded_side_data` for
  the display matrix, `AVChannelLayout`, frame flags rather than
  `key_frame`, filters rather than buffersink options). The Pi build needs
  the `-dev` packages in §3, and `make sysroot` copies libavcodec and
  libavfilter past its 8 MB limit; if the Pi is away, Raspberry Pi OS's
  arm64 `.deb`s of the same version, extracted over a symlinked copy of the
  sysroot (`cp -as`), cross-build the same, and `qemu-aarch64-static -L`
  runs rpod-convert against them.
- swscale's SIMD writes past the end of a row that isn't a multiple of its
  blocks: give it padded, aligned rows (`FFALIGN(w * bpp, 64)`). A 136-wide
  portrait poster corrupted the heap without it -- invisible under ASan,
  which doesn't instrument libswscale.
- The page's Content-Security-Policy needs `media-src 'self' blob:`: the
  upload sorter plays an `.mp4` from a `blob:` URL to see whether it has a
  picture, and without it every `.mp4` would look like a video.
- Playing a video changes MPD's queue (like Music Quiz), so test it against
  a private MPD; `@name` (an abstract socket) gets round the 107-byte path
  limit in the scratchpad.

## Repo layout

See `docs/PLAN.md` §2 for the full annotated tree.

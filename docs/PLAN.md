# rPod — Build & Implementation Plan

A Raspberry Pi Zero 2 W music player using an original 4th-gen iPod click wheel,
a Waveshare 2" ST7789V LCD, and Bluetooth audio. The wired I²S DAC and the
USB-C gadget (mass storage / NCM) are deferred: see §9, "Deferred".

This document is the working spec. It is written to be handed to Claude Code as
project context. Read it fully before writing code.

---

## 0. Ground rules for the implementer

- **Target hardware is a Pi Zero 2 W (BCM2710A1, quad Cortex-A53, 512 MB RAM).**
  RAM is the binding constraint, not CPU. Do not assume a desktop-class budget.
- **No X11, no Wayland, no desktop environment.** The UI renders directly to
  DRM or framebuffer. If you find yourself installing `xorg`, stop.
- **C is the primary language** for the UI and input daemon. Python is
  acceptable for build tooling and one-off scripts only.
- **Every phase below has an acceptance test.** Do not move to the next phase
  until the current one passes on real hardware.
- **Assume the developer is on Arch Linux** with the Pi reachable at
  `rpod.local` over USB-NCM or Wi-Fi.

---

## 1. Hardware

### 1.1 Bill of materials

| Part | Role | Notes |
|---|---|---|
| Raspberry Pi Zero 2 W | Main SoC | Not Zero W — need the quad A53 for FLAC decode headroom |
| Original 4th-gen iPod click wheel | Input | See §4 for protocol. Generation matters. |
| Waveshare 2" LCD (ST7789V, 240×320) | Display | SPI, 4-wire mode |
| PCM5102A breakout | I²S DAC | Forgiving, no I²C config needed, integrated charge pump |
| TPA6132A2 (optional) | Headphone amp | Only needed for >32 Ω cans |
| PiSugar 2 | Battery, charger, boost, RTC | **Fitted now**, under the Zero 2 W, in place of the next four rows — §7.6 |
| BQ24074 | Charger + power path | Power path is mandatory — see §7 |
| TPS61030 or TPS613222 | Boost 3.7 V → 5 V | Size for 1.5 A peak |
| MAX17048 | Fuel gauge | I²C, gives real % rather than a voltage guess |
| Li-ion cell, 3000–5000 mAh | Power | Single cell |
| USB-C receptacle | Charge + data | 5.1 kΩ on each of CC1/CC2 to GND, separately |
| ERM/LRA vibration motor + driver | Haptics | Click wheel scroll feedback |

Wiring is brought up first on a **Raspberry Pi 3B breadboard dev board** (same
40-pin header and GPIO map as below), until it moves into the Zero 2 W build.
It boots the same config with two differences, noted at the top of
`system/config.txt.d/rpod.txt`.

### 1.2 GPIO pin map

This map is conflict-checked. Do not reassign without re-checking peripheral
overlap (SPI0, PCM/I²S, and PWM all have fixed pin options).

| GPIO | Function | Peripheral |
|---|---|---|
| 2 | I²C SDA | PiSugar 2: IP5209 power chip (0x75), SD3078 RTC (0x32) — §7.6 |
| 3 | I²C SCL | PiSugar 2 |
| 8 | SPI0 CE0 | LCD chip select |
| 10 | SPI0 MOSI | LCD data |
| 11 | SPI0 SCLK | LCD clock |
| 24 | LCD D/C | GPIO out |
| 27 | LCD RESET | GPIO out |
| 13 | LCD backlight | Hardware PWM1 |
| 18 | I²S BCK | PCM_CLK |
| 19 | I²S LRCK | PCM_FS |
| 21 | I²S DIN | PCM_DOUT |
| 23 | Click wheel CLOCK | GPIO in, pull-up |
| 25 | Click wheel DATA | GPIO in, pull-up |
| 26 | Haptic motor | GPIO out |
| 22 | USB VBUS sense | GPIO in, via 100k/100k divider |
| 16 | Hold switch | GPIO in, pull-up |
| 17 | Power button | GPIO in, pull-up |

Notes:
- GPIO 23/25/26 deliberately match the `sPot` reference wiring so its schematic
  can be used as-is for the wheel.
- Backlight uses PWM1 on GPIO 13. **Do not** use GPIO 12 or 18 — 18 is I²S.
- LCD MISO is unconnected; the ST7789V read path is not used.

---

## 2. Repository layout

```
rpod/
├── CLAUDE.md                 # implementer context, points at this doc
├── README.md
├── Makefile                  # top-level: build, deploy, deploy-run
├── docs/
│   ├── PLAN.md               # this file
│   ├── clickwheel-protocol.md
│   ├── wiring.md
│   └── wiring-zero2w.svg     # Pi Zero 2 W -> display + wheel (tools/wiring-diagram.py)
├── src/
│   ├── main.c                # event loop, app entry
│   ├── input/
│   │   ├── input.c/.h        # what backends feed: buttons, rotation, sleep
│   │   ├── gestures.c/.h     # taps vs holds, seek repeat, wake (§8.2)
│   │   ├── encoder.c/.h      # the LVGL encoder indev
│   │   ├── wheel_input.c/.h  # click wheel client: acceleration, scrub (§8.2)
│   │   └── wheel_tuning.c/.h # the acceleration curve's numbers, tunable live (§8.2)
│   ├── ui/
│   │   ├── screens/          # one file per screen
│   │   ├── theme.c/.h        # themes: colours, panels, Settings > Theme (§8.4)
│   │   ├── metrics.c/.h      # geometry + type scale, per theme
│   │   ├── classic.c/.h      # the iPod 4G look: its fonts, icons, scrollbar, slider (§8.4)
│   │   ├── alpha_sort.c/.h   # iPod-style A-Z order + letter groups (§8.2)
│   │   ├── scrub.c/.h        # alphabet scrub: jump dispatch + big letter (§8.2)
│   │   ├── hud.c/.h          # status-bar pill: volume, AirPods messages (§6.3)
│   │   ├── volume_control.c/.h # wheel + headset buttons -> MPD volume (§6.3)
│   │   ├── seek_control.c/.h # Next/Prev held: silent, gliding scan (§8.2)
│   │   ├── backlight.c/.h    # backlight timer: screen off when idle (§8.3)
│   │   ├── sleep_timer.c/.h  # Settings > Sleep Timer (§8.1)
│   │   ├── alarm.c/.h        # Extras > Clock > Alarm Clock: time, sound, fade-in (§8.5)
│   │   ├── big_digits.c/.h   # clock-sized numerals: the iPod's, or Montserrat 64
│   │   ├── spinner.c/.h      # numbers set a field at a time with the wheel (§8.5)
│   │   ├── cover_art.c/.h    # JPEG/PNG cover -> small RGB565 tile
│   │   ├── cover_cache.c/.h  # covers off the UI thread, cached on disk (§6.4)
│   │   ├── airpods_notify.c/.h # AirPods battery card + HUD messages (§6.3)
│   │   ├── airpods_art.c/.h  # AirPods glyph + battery card pictures, per theme
│   │   ├── airpods_art_data.c # their photos + glyph (tools/airpods-art.py)
│   │   ├── control.c/.h      # the settings socket the web UI talks to (§8.6)
│   │   └── lvgl_port.c       # LVGL init + display/input driver binding
│   ├── video/                # videos (§8.7)
│   │   ├── video_decoder.c/.h # pictures, decoded on a thread (FFmpeg)
│   │   ├── media_clock.c/.h  # MPD's position, run as a smooth clock
│   │   ├── av_sync.c/.h      # how far behind MPD the sound is heard
│   │   ├── video_library.c/.h # the videos folder's list
│   │   └── mp4_info.c/.h     # an MP4's length and size, from its header
│   ├── ha/                   # Home Assistant (§8.8)
│   │   ├── ha.c/.h           # the REST client, on a worker thread, and its state
│   │   └── json_read.c/.h    # a small JSON reader for its replies (`make test`)
│   ├── games/                # pure game logic, unit-tested (§8.5)
│   │   ├── brick.c/.h        # Brick's rules and physics
│   │   ├── quiz.c/.h         # Music Quiz's rounds and scoring
│   │   └── scores.c/.h       # best scores, persisted
│   ├── audio/
│   │   ├── mpd_client.c/.h   # libmpdclient wrapper
│   │   ├── embedded_art.c/.h # FLAC cover read straight off disk, not via MPD
│   │   ├── bluetooth.c/.h    # BlueZ client over sd-bus, pairing agent (§6.3)
│   │   ├── avrcp_volume.c/.h # headset volume buttons -> volume steps (§6.3)
│   │   ├── aap.c/.h          # AirPods' accessory protocol, encode/decode (§6.3)
│   │   ├── airpods.c/.h      # AirPods extras: session, ear detection, ducking
│   │   ├── sinks.c/.h        # Bluetooth + AirPlay sinks, the default (§6.3b)
│   │   └── sinks_parse.c/.h  # pw-dump -> sinks (tests/test_sinks.c)
│   ├── power/
│   │   ├── battery.c/.h      # the PiSugar 2's IP5209 over I²C, on a thread (§7.6)
│   │   ├── gauge.c/.h        # its registers -> a steady level (`make test`)
│   │   └── usb_gadget.c/.h   # VBUS state machine, LUN bind/unbind
│   └── util/
├── daemon/
│   ├── rpod-wheel.c          # standalone root process, pigpio + Unix socket
│   └── rpod-splash.c         # the boot logo, up as soon as the panel's driver is (§3)
├── web/                      # the web UI (§8.6)
│   ├── rpod_web.c            # rpod-web: its own process, a thread per request
│   ├── http.c/.h             # just enough HTTP/1.1, uploads streamed
│   ├── api.c/.h              # the JSON API and who may call it
│   ├── library.c/.h          # the music directory: list, upload, rename, delete
│   ├── videos.c/.h           # the videos folder: uploads converted one at a time (§8.7)
│   ├── rpod_convert.c        # rpod-convert: any video -> the kind rPod plays
│   ├── control_client.c/.h   # rPod's settings socket, from the other side
│   ├── json.c/.h             # a JSON writer
│   └── static/               # the page: HTML, CSS, JS, icons, Montserrat
├── system/
│   ├── setup.sh              # `make pi-setup`: a fresh Pi's base system (§3)
│   ├── boot/                 # boot time: services off, zram, kernel hook (§3)
│   ├── bluetooth/            # BlueZ + system-wide PipeWire setup (§6.3)
│   ├── owntone/              # AirPlay 2 through OwnTone (§6.3b)
│   ├── tmpfiles.d/           # /run/mpd/airplay, the AirPlay feed's folder
│   ├── pisugar/              # the PiSugar 2: power policy, RTC, polkit (§7.6)
│   ├── mpd/                  # /etc/mpd.conf (§6.2)
│   ├── wireplumber/          # WirePlumber's rpod profile (§6.3)
│   ├── config.txt.d/         # dtoverlay fragments
│   ├── overlays/             # rPod's own device-tree overlays (panel timing)
│   ├── systemd/              # unit files
│   ├── udev/
│   └── gadget/               # configfs setup script
├── tests/                    # host-run unit tests (`make test`)
├── tools/
│   ├── wheel-sniff.c         # raw 32-bit packet logger for §4.3
│   ├── fake-airpods.py       # AAP peer for the sim (§6.3)
│   ├── airpods-art.py        # AirPods pictures from freely licensed sources (§6.3)
│   ├── wiring-diagram.py     # draws docs/wiring-zero2w.svg
│   └── sim/                  # SDL harness to run UI on the dev machine
└── third_party/
    └── lvgl/                 # submodule, v9.x
```

---

## 3. System base

**OS:** Raspberry Pi OS **Lite, 64-bit** (Bookworm or later; the Zero 2 W
runs trixie). No desktop.

`make pi-setup` (`system/setup.sh`) does this whole section on a fresh
install, Bluetooth (§6.3) included; then reboot, `make sysroot`, and deploy.

**`/boot/firmware/config.txt` additions** (`system/config.txt.d/rpod.txt`;
`hifiberry-dac` and `dwc2` are commented out there until the DAC and the USB
gadget are wired, §9 "Deferred"):

```ini
dtparam=spi=on
dtparam=i2c_arm=on
dtparam=audio=off
dtoverlay=i2c-rtc,sd3078
dtoverlay=hifiberry-dac
dtoverlay=dwc2,dr_mode=peripheral
gpu_mem=32
disable_splash=1
boot_delay=0
auto_initramfs=0
camera_auto_detect=0
display_auto_detect=0
```

`hifiberry-dac` is the correct overlay for a PCM5102A — it is a plain I²S
slave with no control interface, same as the HiFiBerry DAC.

**`cmdline.txt`:** append `modules-load=dwc2` and remove `console=serial0` if
you want the UART pins back. Add `quiet` and `logo.nologo`,
`initcall_blacklist=init_kprobe_trace driver_async_probe=dwc_otg` (boot time,
below), and `fbcon=map:9`. That last one maps the kernel console to a
framebuffer that doesn't exist, so it never binds to the panel's. Otherwise,
whenever the panel ends up as `/dev/fb0` (e.g. headless, with no vc4 HDMI
fbdev ahead of it), the console text and blinking cursor draw over the UI.

The panel's `/dev/fbN` index isn't stable for the same reason, so
`system/udev/99-rpod-panel.rules` gives it a fixed `/dev/rpod-panel` symlink;
set `RPOD_FB=/dev/rpod-panel` in `/etc/rpod/env`.

**Partition layout on the SD card:**

| Partition | FS | Purpose |
|---|---|---|
| p1 | FAT32 | boot firmware |
| p2 | ext4 | rootfs |
| p3 | exFAT | **music library — exposed over USB, see §7** |

The music partition must be separate. Do not put music on the rootfs.
Videos (§8.7) live beside the library, not in MPD's `music_directory`:
`/media/videos` on the dev board, a `videos/` folder on the partition once
it's mounted with `music/` beside it.

**Packages:**

```
build-essential pkg-config git
libpigpio-dev
libdrm-dev libmpdclient-dev libcurl4-openssl-dev
libsystemd-dev libjpeg62-turbo-dev zlib1g-dev
libavformat-dev libavcodec-dev libavfilter-dev libswscale-dev libavutil-dev
mpd mpc
exfatprogs
```

**Boot time:** the target is under 8 seconds to first UI frame. Disable
`NetworkManager-wait-online`, `systemd-networkd-wait-online`, `apt-daily`,
`man-db`, and `triggerhappy`. If that still isn't fast enough, the fallback is
never fully powering down — screen off and CPU idle instead of shutdown. Build
the sleep path either way.

Where it stands on the Zero 2 W (2026-10-17, timed from power-on): the rPod
logo is on the panel at ~10 s and the menu at ~22 s, where it used to be a
black panel at 21 s and the menu at 48 s. The firmware's part is ~4.5 s of
that; nothing can draw on the SPI panel before Linux. What it took:

- **The logo.** `rpod-splash.service` loads the panel's driver itself as
  userspace starts, rather than waiting for udev's coldplug, and
  `daemon/rpod-splash.c` draws the wordmark (dark, or light for Classic) the
  moment fbtft registers the framebuffer: ~3 s after the kernel starts
  `init`. The panel holds it until rpod's first frame. rpod.service is
  ordered after it, so it can't land on the UI; it's `RemainAfterExit`,
  since every later unit start pulls `sysinit.target` in again and a plain
  oneshot would draw again.
- **Firmware, 7.5 → 4.5 s.** No initramfs (`auto_initramfs=0`): the kernel
  has ext4 and the SD driver built in, and loading and unpacking the 11 MB
  image cost ~2.5 s. `kernel8.img` stored uncompressed: the firmware took
  2.5 s to inflate the 10 MB gzip, against ~1.9 s to read 25 MB plain.
  `system/boot/rpod-kernel` is a kernel hook that inflates it again after
  every kernel update; if it ever doesn't, the gzipped one still boots. No
  camera or DSI display probing.
- **Kernel, to `init` in 2.3 s.** `initcall_blacklist=init_kprobe_trace`
  skips tracefs's `kprobe_events` (1.1 s), and `driver_async_probe=dwc_otg`
  runs the USB controller's 0.9 s of setup alongside the rest (USB still
  works). Found with `initcall_debug`.
- **systemd.** cloud-init is off and its generator masked (it held systemd
  up 3.5 s). Raspberry Pi OS's `rpi-swap` generator is a Python script
  (1.5 s per boot and per daemon-reload) and backs zram with a swap file on
  the card, attached on the critical path: masked, with zram set up by
  zram-generator directly, RAM-sized, zstd. `/boot/firmware` is an
  automount, idle-unmounted after a minute: its fsck sat on the critical
  path (1.2 s), and unmounted, a power cut can't leave it dirty.
  `keyboard-setup` and `e2scrub_reap` are off.
- **MPD and NetworkManager.** Debian's mpd.service waits for
  network.target, which MPD (on a Unix socket) doesn't need: a copy without
  it is in `/etc` (re-run `make boot-setup` after an MPD upgrade). Raspberry
  Pi OS patches NetworkManager to run `netplan generate` — Python, then a
  daemon-reload — every time it loads its connections, four times a boot,
  ~10 s of the CPU and of systemd itself. It's ordered after rpod.service,
  which is `Type=notify` and reports ready after its first frame
  (`src/app.c`), so it no longer competes with the UI. Wi-Fi still comes up
  sooner than it did (~35 s from power-on, from ~41).

What's left is MPD itself: Debian's build links 328 shared libraries, and
at boot it takes 7–9 s to start, ~4 s of that reading them off the card and
~2.4 s linking them (warm). Reading them in early doesn't help — it's the
same I/O, and it just ran alongside MPD. The ways on: an MPD built with only
the plugins rPod uses, or rpod not waiting for MPD (it would have to
reconnect to it, which it can't today).

To try a boot config change without risking the boot, put it in
`/boot/firmware/tryboot.txt` (`cmdline=` names an alternative cmdline file)
and `sudo reboot "0 tryboot"`: the Zero 2 W's firmware reads it for that one
boot, and a power cycle goes back to `config.txt`.

Add zram swap (`zram-tools`, ~256 MB, lz4). 512 MB is tight once LVGL's frame
buffers and MPD's cache are resident. Raspberry Pi OS ships zram through
`rpi-swap`; `system/boot/setup.sh` replaces that with zram-generator's own
config, as above.

---

## 4. Click wheel

### 4.1 What it is

The 4th-gen click wheel is a self-contained capacitive sensor that emits a
continuous synchronous serial stream on two lines: `CLOCK` and `DATA`. It is a
3.3 V part. Both lines need pull-ups (internal pull-ups on the Pi are
sufficient). There is no command channel — you only listen.

Packets are 32 bits, sent only when something changes: not repeated while a
button is held or a finger rests on the ring (measured on hardware — see
`docs/clickwheel-protocol.md`).

### 4.2 Protocol as implemented in the reference

From `dupontgu/retro-ipod-spotify-client/clickwheel/click.c` (Apache-2.0):

- Sample `DATA` on the **rising edge** of `CLOCK`.
- A run of 32 consecutive `1` bits means idle / inter-packet gap. Reset the bit
  index and stop recording.
- Recording begins on the first `0` bit after idle.
- Every valid packet begins with the pattern `0b01101`.
- After 32 bits are collected, parse and reset.

Field positions **as used by the reference code**:

| Field | Bit |
|---|---|
| Center button | 7 |
| Right button | 8 |
| Left button | 9 |
| Down button | 10 |
| Up button | 11 |
| Wheel touched | 29 |
| Wheel position | bits 16–23, `(packet >> 16) & 0xFF` |

Wheel position is an absolute value around the ring, not a delta. Compute
deltas yourself and handle wraparound: a jump from the top of the range back
to 0 is forward motion, not a near-full-turn reverse. (Measured: 96
positions per turn, 0–95, not the 8-bit range the reference implies.)

### 4.3 The bug you must not port

The reference `setBit()` is:

```c
int setBit(int n, int k) { return (n | (1 << (k - 1))); }
```

It is called with `bitIndex` starting at **0**, so the first call evaluates
`1 << -1`, which is undefined behaviour. On ARM the shift count is masked and
the result is 0, so bit 0 is silently never written and every subsequent bit
lands one position lower than its index implies.

The reader, however, uses `(bits >> buttonIndex) & 1` — no off-by-one. The
constants in the table above were therefore derived *empirically against the
buggy writer*, and they only work as a matched pair.

**Do not copy the constants into a correct implementation.** Instead:

1. Write `tools/wheel-sniff.c` first. Correct bit packing (`1 << k`), no
   parsing, just print each completed 32-bit packet as binary plus a timestamp.
2. Run it on hardware. Press each button in isolation, then rotate slowly.
3. Diff the packets to derive the real bit positions for your build.
4. Record the findings in `docs/clickwheel-protocol.md` and use those.

This costs an hour and saves a week of chasing phantom input bugs.

### 4.4 Generation warning

The protocol above is specific to the **4th generation** click wheel. The iPod
Mini, 3rd gen, and 5th gen (Video) wheels differ in packet length and framing.
Confirm which part you physically have before assuming any of this holds. The
sniffer tool in step 1 will tell you immediately — if you never see the
`0b01101` preamble, you have a different generation.

### 4.5 Implementation

`daemon/rpod-wheel.c`:

- Uses `pigpio`'s DMA sampling via `gpioSetGetSamplesFunc()`, reading DATA
  from the **same sample** as each CLOCK rising edge. Don't call
  `gpioRead(DATA)` from a CLOCK alert callback (the reference's approach,
  and this repo's first draft): pigpio delivers callbacks from a buffer of
  samples about a millisecond after the fact, so `gpioRead()` returns DATA's
  level *now*, not at the edge.
- **Set the sample rate before `gpioInitialise()`** with
  `gpioCfgClock(…)`; the 5 µs default will drop edges. Measured on the 3B:
  the narrowest CLOCK pulse is 7 µs, and 1 µs sampling cost ~18% of a core
  even with the wheel idle. The daemon runs at 2 µs (~11%), which still
  samples every half-cycle at least 3 times. `wheel-sniff` stays at 1 µs.
- **The DMA sampling is paced by the PCM or PWM peripheral**, which then
  can't do its normal job: PCM is I²S (the DAC, §6.2), PWM is the Pi 3B's
  headphone jack and hardware-PWM backlight dimming (GPIO 13, §1.2). The
  daemon defaults to PCM, which suits the 3B dev board playing through its
  jack. Once the DAC is wired, set `RPOD_WHEEL_PACING=pwm` in
  `/etc/rpod/env`, and backlight dimming can't then use hardware PWM.
  Resolve that before Phase 3, or take §4.6's RP2040 route, which frees
  both.
- **Call `gpioCfgInterfaces(PI_DISABLE_FIFO_IF | PI_DISABLE_SOCK_IF)`**
  before `gpioInitialise()`. Otherwise the pigpio *library* (not just
  `pigpiod`) opens a root GPIO-control socket on TCP port 8888, reachable
  from the network, plus a `/dev/pigpio` FIFO.
- pigpio requires root for `/dev/mem`. Run this as a separate privileged
  process; the UI stays unprivileged.
- **Call `gpioCfgMemAlloc(PI_MEM_ALLOC_PAGEMAP)` before `gpioInitialise()`.**
  With `dtoverlay=vc4-kms-v3d` active (§3's config.txt), pigpio's default
  mailbox-based DMA memory allocation fails outright —
  `initMboxBlock: init mbox zaps failed`, `gpioInitialise` returns < 0 —
  because the DRM/KMS driver holds the legacy GPU memory pool pigpio's
  mailbox path needs, independent of `gpu_mem=`. PAGEMAP mode allocates
  differently and sidesteps this entirely. Confirmed on hardware.
- pigpio isn't packaged for this OS (only the `pigpiod_if2` daemon-client
  split is available via apt) — build the classic library from source
  (`github.com/joan2937/pigpio`, `make && sudo make install`) directly on
  the target device rather than cross-compiling.
- Publish decoded events over a **Unix domain socket** at
  `/run/rpod/wheel.sock`, not the UDP port 9090 the reference uses. UDP on
  loopback for local IPC is unnecessary and drops packets under load.
- Framing hardening on top of §4.2: a quiet gap longer than any
  intra-packet clock gap abandons a half-received packet, so one dropped
  edge can't shift every later packet by a bit. Packets without the
  preamble are discarded. The wheel only transmits while touched or
  pressed, so if it falls silent with something still down, a watchdog
  reports the release rather than leave the UI with a stuck button.
- Event wire format: a packed struct, not text.

```c
struct rpod_wheel_event {
    uint8_t  type;      // 0=button, 1=wheel, 2=touch
    uint8_t  code;      // button id, or unused
    int8_t   value;     // press/release, or wheel delta (signed, wrap-corrected)
    uint8_t  _pad;
    uint32_t position;  // absolute wheel position
    uint64_t timestamp_us;
};
```

- Only one pigpio process can run at a time: stop `rpod-wheel` before
  running `wheel-sniff`.
- The UI side is `src/input/wheel_input.c`: an unprivileged socket client
  that feeds the shared encoder indev (§5.4) and applies scroll
  acceleration (§8.2). Because it's only a socket client, the desktop
  simulator can use the real wheel too, over an SSH-forwarded socket (see
  `tools/sim/sim_main.c`).
- Haptics: fire a short pulse on GPIO 26 via a prebuilt `pigpio` waveform. The
  reference fires every second position; make the divisor a runtime tunable —
  it interacts with scroll acceleration and will need tuning by feel.
- Respect the hold switch (GPIO 16): when engaged, keep decoding but suppress
  event emission.

### 4.6 Fallback if timing proves unreliable

If pigpio edge detection drops packets under audio load, move the decoder to a
dedicated RP2040 talking to the Pi over UART. The RP2040's PIO handles this
perfectly and deterministically. The same MCU can then double as the power
controller (power button, safe shutdown signalling, battery gating). Treat this
as a planned escape hatch, not a failure — design the socket protocol so the
UI cannot tell which backend is feeding it.

---

## 5. Display

**Panel:** Waveshare 2", ST7789V, 240×320 native (portrait) raster, SPI
4-wire. Driven in **landscape** (320×240 logical framebuffer, via rotation)
— click-wheel iPods (4th gen, Photo, Video/Classic, Mini) all had landscape
screens sitting above the wheel, despite the device body being portrait
overall. An earlier draft of this doc had this backwards; if you find other
leftover portrait assumptions, fix them too.

Two driver paths. Try them in this order.

### 5.1 Preferred: `panel-mipi-dbi` (DRM)

The modern mainline path. Gives you `/dev/dri/card1` and a real DRM/KMS device,
which LVGL v9 can target directly. Requires a firmware blob describing the
panel init sequence, generated with `mipi-dbi-cmd` from a plain-text command
list, placed in `/lib/firmware/`.

```ini
dtoverlay=mipi-dbi-spi,spi0-0,speed=62500000
dtparam=width=320,height=240
dtparam=reset-gpio=27,dc-gpio=24,backlight-gpio=13
```

Unverified — this path hasn't been tried yet (§5.2's fbtft fallback is what's
actually running). May need an explicit rotation param once attempted; check
against whatever `panel-mipi-dbi`'s current bindings call it.

### 5.2 Fallback: `fbtft`

Older, in staging, deprecated — but a one-line overlay and it works today.
Produces `/dev/fb1`, which LVGL's fbdev backend drives fine.

```ini
dtoverlay=fbtft,spi0-0,st7789v,width=240,height=320,rotate=0
dtparam=reset_pin=27,dc_pin=24,led_pin=13
dtparam=speed=62500000,fps=60,txbuflen=32768
dtoverlay=rpod-panel
core_freq_min=400
```

Keep it split like this: the firmware silently truncates `config.txt` lines
at 98 characters, and the original one-line form lost its `speed=` param
that way (see §5.3). `dtparam=` lines apply to the `dtoverlay=` above them.

`width`/`height` here describe the panel's native (portrait) raster, and
`rotate=0` keeps the framebuffer that way: `src/ui/lvgl_port.c` renders the
320×240 landscape UI and rotates it into the framebuffer itself, because
fbtft's own `rotate=90` made scrolling tear diagonally (§5.3). The picture
lands where `rotate=90` put it on the dev board, read upright there;
`rotate=180` is the same picture, pushed in the opposite direction (§5.3).
If a final build needs it turned 180°, that's a change to the mapping in
`lvgl_port.c`'s `push_rotated()`, not to `rotate=` — confirm against real
text on the panel (a solid colour fill can't tell you the read direction).
`rotate=90`/`270` still work, copied straight through, tearing and all.

`rpod-panel` (`system/overlays/rpod-panel.dts`, installed by `make
deploy-overlay`) and `core_freq_min=400` are timing fixes, also §5.3.

Start here if §5.1 fights you. Migrating later is a contained change confined
to `lvgl_port.c`.

### 5.3 Performance envelope

240 × 320 × 16 bpp = 153,600 bytes per full frame. At 62.5 MHz SPI that is
~30 fps theoretical, ~25 fps real with DMA overhead. This is fine **provided
you never do full-frame redraws**. Configure LVGL for partial rendering with
two ~40-line draw buffers and let it dirty-rect. Scrolling a list should be
touching a fraction of the panel per frame.

**Measured on real hardware** (fbtft, `rotate=90`): the label+spinner scene
renders at a measured 30 fps, comfortably clearing the Phase 1 accept bar.
That was at 32 MHz, not the requested 62.5 — not the SPI core's doing, but
the one-line overlay above running past the firmware's 98-character line
limit: `sudo vclog -m` showed `Unknown dtparam 'spe' - ignored`, leaving the
overlay's 32 MHz default. With the params split (§5.2) `dmesg` reports 62
MHz, but that's only the requested maximum: the SPI block divides the core
clock by an even number, fixed at boot from the 400 MHz core (8), so it runs
at 50 MHz and a full frame takes **25.4 ms** — measured with `pwrite()` +
`fsync()`, which on a deferred-I/O fb pushes immediately and returns when
the push is done.

Also measured (SPI controller counters in
`/sys/bus/spi/devices/spi0.0/statistics`): on kernel 6.18, fbtft's `write()`
path marks the whole display dirty, so **every push is a full 153,600-byte
frame** whatever changed — partial rendering saves CPU, not SPI. What
matters is handing fbtft whole frames: `src/ui/lvgl_port.c` renders in
LVGL's DIRECT mode and writes each refresh in one `pwrite()`, rather than
row-by-row as each 40-line chunk renders (fbtft's deferred-I/O timer could
fire mid-frame and push half a screen, then the rest — a visible two-stage
wipe on scroll). Two hardware-specific gotchas worth knowing before
touching `src/ui/lv_conf.h` again:

- `LV_MEM_SIZE` needs real headroom above the draw buffers. At 64 KB, the
  two 320-wide partial buffers (51,200 bytes) left too little for LVGL's
  own widget/font/layer allocations, and `LV_USE_ASSERT_MALLOC`'s handler
  (`while(1);`) turned a failed allocation into a silent hang — 100% CPU,
  no crash, no log output (logging's off), screen frozen. 256 KB fixed it;
  512 MB total RAM makes this a non-issue either way, so don't be stingy.
- Write the framebuffer with `pwrite()`, never through an mmap (rPod's port
  does; this was LVGL's `LV_LINUX_FBDEV_MMAP`, now unused). mmap'd writes
  from LVGL's long-running process never reach the
  panel — confirmed the driver mechanism itself is otherwise sound (a
  throwaway test program doing long-lived, rapid, repeated mmap writes at
  LVGL's redraw cadence worked every time), so this looks like a narrow
  staging-driver bug specific to LVGL's exact access pattern, not something
  worth chasing further. Don't switch to mmap without re-verifying on the
  actual panel, not just fps/CPU numbers — a wedged flush still leaves the
  process looking "active."
- Under heavy rapid testing (many opens/mmaps/writes across multiple
  processes in a short window, no reboot in between) the driver's internal
  state can wedge — writes stop reaching the panel with zero kernel-side
  error, everything still "works" from software's point of view. A reboot
  clears it. Don't chase this as a config bug if it happens; just reboot
  and re-test.

**Tearing.** The ST7789V refreshes its glass from its own RAM, one native
(portrait) row at a time, ~60 times a second by default, and the
Waveshare module doesn't break out its TE (tearing effect) line, so pushes
can't be synced to the refresh. With fbtft's `rotate=90`, MADCTL (`MV|MY`)
maps each landscape row fbtft sends onto a native *column*: every push
swept the glass perpendicular to the refresh, so every refresh pass that
overlapped a push showed new pixels on one side of a slanted line and old
ones on the other — a diagonal tear on every frame that moved, very visible
while scrolling. Rotating in software (`rotate=0`, §5.2) makes pushes write
the glass in the order it refreshes, so they only tear when the refresh
overtakes the push partway (or vice versa), and along a straight line.
How often that happens depends on the two speeds: tearing hits
(push time − sweep time) / refresh period of pushes. At the default refresh
(FRCTRL2 `0x0F`, ~62 Hz, 320 rows swept in ~16 ms) against a 25.4 ms push,
that's about 60%. `system/overlays/rpod-panel.dts` sets FRCTRL2 `0x1F`, the
slowest normal-mode refresh (~41 Hz, ~24 ms sweep), cutting it to roughly
5% — that overlay replaces fbtft's whole init sequence, so diff it against
`fb_st7789v.c`'s `init_display()` after kernel upgrades. If scrolling still
shows a straight vertical tear on *most* frames, the panel refreshes the
other way: switch to `rotate=180`.

That match only holds with the core clock pinned (`core_freq_min=400`, §5.2).
Left to itself, the firmware drops the core to 275 MHz whenever the CPU is
near idle — light UI work like scrolling included — and the SPI divider
doesn't follow, so the SPI clock fell to 34 MHz and pushes to 37.1 ms,
flipping between the two run to run. The pin costs a little idle power.

rPod `fsync()`s every frame: the push starts at once instead of after
fbtft's deferred-I/O delay (`fps=`), and LVGL can't overwrite the
framebuffer while a push is still reading it out, which used to tear the
bottom of a frame whenever the 33 ms refresh period landed inside a
delay + push.

Set orientation to landscape (320×240 logical). Confirm the panel's
column/row offsets — many ST7789 breakouts need an offset because the
controller supports 240×320 but the glass is smaller. This one is full
240×320, so offsets should be zero, but verify with a full-screen fill of a
known colour before trusting it.

### 5.4 UI framework

**LVGL v9**, vendored as a submodule under `third_party/`.

- Display driver: DRM (`lv_linux_drm`) or fbdev (`lv_linux_fbdev`).
- Input driver: custom, fed from the wheel socket. Register the wheel as an
  `LV_INDEV_TYPE_ENCODER` — this is exactly what LVGL's encoder input type was
  designed for, and it gives you group-based focus navigation for free.
- Center button maps to encoder press. Menu/back, prev, next, play/pause map to
  application-level shortcuts, not encoder events.

Build a **desktop simulator harness** in `tools/sim/` using LVGL's SDL backend
on day one. You will iterate on the UI far faster on the dev machine than over
SSH, and it makes the UI testable without hardware.

---

## 6. Audio

### 6.1 Don't write a music player

Use **MPD** as the playback engine and make rPod an MPD client via
`libmpdclient`. This is not a shortcut — it is the correct decomposition. MPD
gives you, already debugged:

- FLAC, ALAC, MP3, AAC, Ogg, WavPack decoding
- Gapless playback and crossfade
- ReplayGain
- A tag database with fast queries
- Multiple named outputs with runtime switching
- A stable, documented, well-supported C client library

Writing your own decode and mixing path is months of work to arrive somewhere
worse. Spend that time on the UI, which is the actual product.

Configure MPD with `--no-daemon` under a systemd unit, bound to a Unix socket,
no network listener.

### 6.2 Two outputs

```
# /etc/mpd.conf — abridged

audio_output {
    type    "alsa"
    name    "DAC"
    device  "hw:CARD=sndrpihifiberry,DEV=0"
    mixer_type "software"
    auto_resample  "no"
    auto_format    "no"
    auto_channels  "no"
}

audio_output {
    type    "pipewire"
    name    "Bluetooth"
    remote  "/run/pipewire/pipewire-0"   # system-wide instance, §6.3
    mixer_type "software"   # same volume curve as the DAC; see §6.3
    enabled "no"
}

audio_output {
    type    "fifo"
    name    "Visualizer feed"
    path    "/run/mpd/visualizer.fifo"
    format  "44100:16:2"
}
```

The `auto_*` settings off is what makes the DAC path bit-perfect — MPD hands
the DAC the source rate and lets it reclock rather than resampling to a fixed
rate.

Switching outputs is `mpd_run_enable_output()` / `disable_output()`. Only one
enabled at a time. Expose this in the UI as a source picker.

The third output is a standing raw-PCM tap for the Now Playing screen's
visualizer (`src/audio/visualizer.c`) — always enabled, not part of the
source picker above. `format` is fixed regardless of the source file's own
format so the visualizer only ever has to handle one PCM layout; must match
`RPOD_VIS_SAMPLE_RATE`/`RPOD_VIS_CHANNELS` in `src/audio/visualizer.h`. See
`tools/sim/mpd-dev.conf.in` for the same output wired up for the desktop
simulator.

### 6.3 Bluetooth and AirPods

Phase 4, and explicitly optional. BlueZ + PipeWire handles A2DP to AirPods with
no custom code — you get audio, and that alone may be enough.

**System setup** (`make bluetooth-setup`, which runs
`system/bluetooth/setup.sh` on the Pi). MPD is a system service with no user
session, so PipeWire runs **system-wide** instead of per-user. That means
upstream's system-mode units (`system/systemd/pipewire.{socket,service}`,
`pipewire-manager.socket`, `wireplumber.service`), which Debian doesn't
package. They run as a `pipewire` system user (in the `bluetooth` group), with
the socket at `/run/pipewire/pipewire-0`. `mpd` joins the `pipewire` group,
and the `Bluetooth` output in `system/mpd/mpd.conf` points `remote` at that
socket. WirePlumber runs the `rpod` profile
(`system/wireplumber/wireplumber.conf.d/rpod.conf`): upstream's stateless
systemwide `main-embedded`, with the ALSA and video monitors off. PipeWire must
never open the sound card, because the DAC path stays MPD → ALSA `hw:`
direct (§6.2). The per-user PipeWire units are masked globally so an SSH
session can't spawn a second instance. Packages are installed with
`--no-install-recommends` (no `pipewire-pulse`).

**Volume** (`system/mpd/mpd.conf`, and `wireplumber.settings` in the `rpod`
profile). MPD's software mixer is the only volume control. The pipewire
mixer's cubic curve made the bottom quarter of the range inaudible (4% =
−84 dB). The profile also pins a headset's own volume at a fixed ceiling via
`default-sink-volume`. PipeWire sends the headset cbrt(volume) as its AVRCP
absolute volume, so 0.421875 is 75% of the headset's own scale. It's
calibrated on AirPods Pro so the wheel tracks an iPhone's slider through
normal listening levels: iPhone 4–6 of 16 sounds like wheel clicks 6–10.
WirePlumber's stateless default of 0.064 (40%) made MPD's 100% too quiet, and
1.0 pushed comfortable listening down to the wheel's bottom steps.

A headset's own volume buttons feed the same control
(`src/audio/avrcp_volume.c`, `src/ui/volume_control.c`). AirPods report a
stem swipe, and most headphones their buttons, by changing their AVRCP
absolute volume, which BlueZ shows as `MediaTransport1.Volume` (0–127).
rPod turns each change into wheel-sized steps on MPD's volume (8 AVRCP units
a step, iOS's 1/16th), sets the headset back to its pinned level, and shows
the HUD. Changes while not streaming, or within 2.5 s of streaming starting,
are setup (PipeWire applying its level, the headset reporting its own), so
they just move the pin. If BlueZ refuses the set, that transport's buttons
are left alone rather than counted twice. `RPOD_BT_DEBUG=1` logs each
change.

The volume is also remembered per device (`src/audio/volume_memory.c`): one
level for each Bluetooth audio device by address, and one for "wired" when
none is connected. Each level is saved in `/var/lib/rpod/volumes`. On a BlueZ
connect or disconnect, the outgoing device keeps its level and the incoming
one gets its own back. The switch lands before A2DP audio flows, so a
speaker's level never reaches the AirPods. A device seen for the first time
keeps the current level, capped at 50%.

**Pairing UI** (Settings → Bluetooth, `src/ui/screens/bluetooth_screens.c`
on top of `src/audio/bluetooth.c`). The UI talks to bluetoothd over the
system D-Bus with sd-bus (so the sysroot needs `libsystemd-dev`). Every call
is async, and an lv_timer drains the connection, so a 10-second `Pair()`
never blocks the wheel. The screen lists the adapter's power toggle, the
paired devices (select one to Connect/Disconnect or Forget it), and "Search
for Devices". Search runs BR/EDR-only discovery while that screen is open.
It lists unpaired *audio* devices only (Audio/Video class, an `audio-*`
icon, or an A2DP-sink/headset UUID), and selecting one runs pair → trust →
connect. Discovery is paused during the pairing so inquiry doesn't fight
connection setup. The screen also reports what's missing: "The
Bluetooth service isn't running", "No Bluetooth adapter found", or "Blocked
by rfkill".

**Pairing agent.** rPod registers a NoInputNoOutput agent ("just works",
which is what headphones do; a legacy PIN-only device fails) and, on the
device, claims BlueZ's *default* agent. That's what makes pairings stick:
with no agent registered and `AlwaysPairable = false` (BlueZ's default),
BlueZ leaves the adapter non-bondable, and the kernel then pairs with "no
bonding". The link key is dropped at disconnect, BlueZ shows `Paired: yes`
but `Bonded: no`, and the headphones need pairing mode every time, which is
exactly what the first builds did. The agent refuses code requests and only
confirms pairings or authorizes untrusted devices while the user is pairing
(the search screen is open, or a `Pair()` is running). The sim registers
the agent but doesn't claim the default, which would take over the
desktop's pairing prompts (`RPOD_BT_DEFAULT_AGENT=1` claims it, for a
dbusmock bus). A device paired before this shows "Not saved - forget, then
pair again".

**Reconnecting.** Bonded and trusted headphones reconnect by themselves:
AirPods page the last device they were used with when the case opens or
they go in an ear. rPod also asks for them 5 s after Bluetooth comes up
(rPod starting, bluetoothd restarting, the adapter powering on; the delay
lets WirePlumber register its A2DP endpoints at boot). That's one try per
paired audio device, AirPods first, until one connects, and failures stay
quiet since nobody asked. rPod doesn't page them periodically after that,
because it would fight an iPhone the AirPods were last used with.

To pair AirPods, put them in pairing mode first (case open, hold the
button until the light flashes white). After connecting, the headset is a
sink; choose between it and any AirPlay speaker from Now Playing (§6.3b).
`bluetoothctl` on the Pi still works as a
fallback (`scan on`, `pair <MAC>`, `trust <MAC>`, `connect <MAC>`). To
inspect PipeWire, run `sudo PIPEWIRE_RUNTIME_DIR=/run/pipewire wpctl status`.

Still to verify on hardware: what MPD's `Bluetooth` output does when the
headset disconnects mid-track. With the ALSA monitor off, no other sink
exists, and the stream may stall rather than error out. The Phase 6 UI should
fall back to the wired output on disconnect either way.

**AirPods extras** (`src/audio/airpods.c`, the LibrePods spike). AirPods
speak Apple's accessory protocol (AAP) on a second L2CAP channel, PSM
0x1001, beside A2DP. It carries battery for each bud and the case, in-ear
state, noise control, Conversation Awareness, stem presses, and the AirPods'
own settings. LibrePods (github.com/kavishdevar/librepods) documents the
protocol. Its Linux app is a desktop tray app (Qt, being rewritten in Rust)
that drives media through MPRIS and PulseAudio on a session bus, which the Pi
doesn't have, and it's GPL-3.0. So rPod reimplements the protocol in C from
LibrePods' notes instead of running or porting it. `src/audio/aap.c` encodes
and decodes packets (`make test` checks it against real captures).
`src/audio/airpods.c` runs the session on the LVGL thread: a non-blocking
socket polled from an lv_timer, like `bluetooth.c`'s bus.

Whichever connected BlueZ device lists the AAP service UUID gets the channel,
1.5 s after it connects so A2DP comes up first. Setup is the handshake, then
feature flags (all on: that unlocks Adaptive, and Conversation Awareness
during playback), then the notification request. The AirPods answer it with
battery, in-ear state and every setting. A dropped channel reopens with
backoff while the device stays connected. On top of that, rPod does what an
iPhone does:

- Taking a bud out pauses MPD, and putting it back resumes. Only what ear
  detection paused, only once as many buds are back in as when it paused,
  only while MPD's `Bluetooth` output is on, and only with the AirPods'
  Automatic Ear Detection on.
- Conversation Awareness ducks the volume while the wearer talks. Levels 1-2
  drop MPD's volume to 20% of where it was, 3-7 ease it back, 8-9 restore
  it. `volume_memory.c` does the ducking, so the lowered level is never
  remembered as the AirPods' own, and turning the wheel mid-duck ends it.
- Stem presses come over AAP instead of AVRCP, which nothing on rPod listens
  to: press for play/pause, double press for next, triple press for
  previous. Press-and-hold stays the AirPods' own (noise control cycling).

What an iPhone shows when they do something
(`src/ui/airpods_notify.c`, `src/ui/hud.c`):

- A battery card slides up from the bottom when the AirPods connect, which,
  now that they reconnect by themselves, means when the case opens near
  rPod. It shows a drawing of the buds with their charge (one figure when
  the buds are within 10% of each other, else L and R) and of the case
  while it's reporting, green with a bolt when charging and red at 20% or
  below. It updates live, and goes away after 6 s or at any turn or press.
  The case reporting again later (a bud put back) shows it again, at most
  every 30 s, and so does a dropped control channel coming back. With
  another overlay open, the same news is a HUD message instead.
- The HUD (the status-bar pill the volume uses) shows the noise control
  mode when it changes, whether from a stem press-and-hold or from
  Settings. It also warns when a bud in use reaches 20% and again at 10%,
  and says when the AirPods disconnect. A message that arrives mid-turn
  waits for the volume face to finish.
- While they're connected, the volume HUD shows an AirPods glyph instead of
  a speaker, and the status bar shows one beside the battery (dim until the
  controls are up). The pictures (`src/ui/airpods_art.c`) follow the
  theme. Glass has photos of AirPods Pro and their case from Wikimedia
  Commons, cut out and scaled down, and MingCute's AirPods icon for the
  glyph. `tools/airpods-art.py` generates them (into
  `airpods_art_data.c`, sources and licences in its header). The classic
  themes have hand-drawn pixel art in the manner of the iPod's own
  bitmaps instead -- 1-px outlines and its greys, pixel-doubled -- with a
  13×9 glyph like its title bar icons and an 11×7 one for the HUD's box.
- AirPods with Find My call themselves "Name - Find My" to non-Apple
  devices. rPod drops the suffix.

The UI is the AirPods' settings page (`src/ui/screens/airpods_screens.c`).
It sits at the top of Settings while they're connected, and AirPods open it
from Settings → Bluetooth. It shows battery and the noise control mode, plus
Adaptive Audio strength, Conversation Awareness, Personalized Volume,
Automatic Ear Detection, which modes press-and-hold cycles through, Off
Listening Mode, press speed, hold duration, one-AirPod noise cancellation,
volume swipe and its speed, and model/serial/version. A row only appears for
a setting the AirPods report, so other models show what they have.

Not done: renaming (needs the search screen's wheel keyboard factored out
into a component), and what LibrePods gates behind VendorID spoofing
(`DeviceID = bluetooth:004C:0000:0000` in BlueZ's `main.conf`): hearing aid,
transparency customisation, Loud Sound Reduction, and multipoint with an
iPhone. Head tracking and spatial audio don't apply to a music player.

For the sim, `tools/fake-airpods.py` plays the AirPods' side over a Unix
socket, and `RPOD_AIRPODS_SOCK=<path>` swaps BlueZ for it (`lid open|close`
exercises the case reporting; the battery card's other triggers are
`battery`, `case` and `drop`).
`RPOD_AIRPODS_DEBUG=1` logs every packet both ways, for checking the
protocol against new firmware.

Verified on AirPods Pro 2 (2026-10-04, Pi 3B): the channel comes up (after
one refused attempt while the AirPods were still busy pairing), and ear
detection pauses and resumes. Still to verify: that Conversation Awareness
levels 3-7 really ramp back up (the 1-2 and 8-9 meanings are documented, the
middle is inferred), and that stem presses arrive over AAP on this firmware
without AVRCP also reaching MPD.

The agent was verified on the Pi 3B (2026-10-04): with it registered,
`Pairable` reads true and `btmgmt info` lists `bondable`, where before it
didn't. Still to verify there: that a fresh pairing shows `Bonded: yes` and
reconnects when the case opens, and that a stem swipe arrives as a
`MediaTransport1.Volume` change BlueZ lets rPod set back.

Be honest in scoping: everything over Bluetooth is lossy regardless. The
lossless story is the headphone jack. Do not let AirPods integration drive
architectural decisions in the audio path.

### 6.3b AirPlay and choosing where to play

MPD has no AirPlay output, and PipeWire's (`libpipewire-module-raop-discover`)
only speaks AirPlay 1, which a HomePod mini on audioOS 27 turned away (`403
Forbidden` to `OPTIONS` and `ANNOUNCE`). So AirPlay is **OwnTone**, which speaks
AirPlay 2, from its own apt archive for Raspberry Pi OS (Debian doesn't
package it; `system/owntone/setup.sh`, `make owntone-setup`, part of
`pi-setup`). It isn't a second music server: MPD's `AirPlay feed` fifo output
(`system/mpd/mpd.conf`, 44100:16:2, in `/run/mpd/airplay` from
`system/tmpfiles.d/rpod-airplay.conf`) is the only thing in OwnTone's library,
and OwnTone plays it, when sound arrives, to the speaker it has selected.
`system/owntone/owntone.conf` keeps it to that: its JSON API on port 3689
trusts localhost only, with no password to get in otherwise, its own MPD
server (TCP 6600) and local audio are off, and so is IPv6 -- the Pi has no
IPv6 address on this Wi-Fi, and PipeWire's sink for the HomePod, found by its
IPv6 address, never connected.

The headset is the other half: MPD's `Bluetooth` output plays into
PipeWire's default sink, and "which headset" is `wpctl set-default`. Exactly
one of MPD's `Bluetooth` and `AirPlay feed` outputs is on, and
`src/audio/sinks.c` is what switches them: choosing a speaker selects it in
OwnTone (`PUT /api/outputs/set`), then turns `Bluetooth` off and the feed on;
choosing the headset does the reverse. The feed has to be off while the
headset plays, or OwnTone would start on it by itself. Every other output
stays on (Settings used to have an Audio Output switch; it's gone, and rPod
enables any output an older build switched off).

Holding Play/Pause for 0.7 s on Now Playing opens **Play On**
(`src/ui/screens/output_screens.c`), a glass popup like Add to Playlist: the headset and the speakers, the
current one checked, refreshed every few seconds so a speaker that turns on
shows up. Anywhere else the hold carries on to sleep at 1.5 s. The list is
`src/audio/sinks.c`: `pw-dump` and OwnTone's `/api/outputs`, and the
switching above, on a worker thread (never the LVGL thread), against
`RPOD_PIPEWIRE_DIR` (`/run/pipewire`, set in `rpod.service`; rpod is in the
`pipewire` group). The sim has no such directory, so it leaves the desktop's
own default sink alone; `RPOD_SINKS_FAKE=1` gives it a made-up headset and
two speakers to click through.

A speaker plays about 1.6 s behind MPD (`RPOD_SINKS_AIRPLAY_DELAY_MS`):
OwnTone's `start_buffer_ms`, cut from 2.25 s to 1 s so a speaker starts
sooner, plus what it reads ahead while it connects. A playing video allows
for that (`src/video/av_sync.c`), and AirPods coming out of ears don't pause
music that's playing on a speaker. The wheel's volume HUD shows a speaker
then, not the AirPods, and `src/audio/volume_memory.c` keeps each speaker's
level under `ap:<OwnTone id>`. MPD's outputs each have their own software
mixer, so `sinks.c` carries the level across when it switches them, and
volume memory then files it under the device it belonged to.

Pausing needs care. MPD's fifo output keeps the fifo open for good, paused or
switched off, so OwnTone never sees it end: a pause is only its feed going
quiet, and 1.5 s later it suspends playback and waits for its buffer to fill
again, which made every resume take 4 s. Worse, that restart can deadlock
OwnTone 29.3: `src/input.c`'s `wait_buffer_ready()` calls into the player
holding `input_buffer.mutex`, while the player waits for that mutex in
`input_read()` (gdb on the Pi, with `owntone-dbgsym`). So `sinks.c` follows
MPD's player (`idle player` on a thread of its own) while a speaker plays, and
on a pause or stop tells OwnTone to stop at once (`PUT /api/player/stop`); the
feed starting again autostarts it afresh. The stop drops what OwnTone had
buffered, which MPD counts as played, so a pause also moves MPD back by the
delay, and play picks up where the sound stopped. A gap MPD makes by itself
(its decoder stalling on the SD card) can still take OwnTone down that path,
so `rpod-owntone-watchdog.timer` restarts it when `/api/outputs` (answered
by the player thread) stops answering; `TimeoutStopSec=10` (its drop-in)
because a deadlocked OwnTone never finishes shutting down. Its database is
WAL with `synchronous` NORMAL: the player thread writes it as playback
starts, and shouldn't wait on the SD card.

Apple's OS 27 speakers check the sender's `User-Agent`: anything that doesn't
call itself `AirPlay/...` gets `403 Forbidden`, even to `GET /info`. OwnTone
29.3 sent `OwnTone/29.3` (its `general` `user_agent`), so `owntone.conf` sets
that to `AirPlay/999.0.0`, which is upstream's fix (commit 34603ba9, issue
#2042; later versions send it to speakers from `airplay_shared` by default).
An Apple TV with "Require Device Verification" on asks for a PIN the first
time, shown on the TV: `curl -X PUT -d '{"pin":"1234"}'
http://127.0.0.1:3689/api/verification` on the Pi while it's up, and OwnTone
keeps the pairing. rPod has no screen for that yet. A speaker that fails
like that, or drops off the network, leaves OwnTone with none selected, and
`sinks.c` (which asks again every 5 s while a speaker is playing) gives the
sound back to the headset.

### 6.4 Library index

MPD's own database is the index. The plan was a parallel SQLite index built
with taglib, on the theory that MPD's database would be awkward to query for a
browsing UI; it wasn't. Every browse screen is one MPD query
(`src/audio/mpd_client.c`), and what MPD doesn't do is done on top of its
results: iPod-style A-Z order and letter groups (`src/ui/alpha_sort.c`), and
the flat Songs list virtualized so 800+ rows scroll smoothly. A second index
would only be one more thing to keep in step with the files.

Cover art is the one thing kept outside MPD (`src/ui/cover_cache.c`): one
decoded tile per album on disk, filled for the whole library in the
background whenever MPD's database changes. A cover comes from the album's
first track, or from the next track that has one -- rips don't always embed
it in every track.

When the USB gadget disconnects (§7) -- the only moment the library can
have changed -- ask MPD to rescan (`update`); the cover cache follows on its
own.

### 6.5 ListenBrainz scrobbling

In scope for v1, unlike §11's streaming-client/Wi-Fi-sync deferrals — this is
a small, one-directional, best-effort side effect of playback (submitting
"now playing"/"listen" events), not a new way of sourcing audio, and it
doesn't need any new UI input.

`src/audio/listenbrainz.c` posts to ListenBrainz's `submit-listens` API from
a background thread (never blocking the LVGL tick); `src/audio/scrobbler.c`
polls MPD status once a second and fires a submission once a track has
played past the standard threshold (half its duration or 4 minutes,
whichever is lower, and only for tracks longer than 30s).

No click-wheel text entry exists yet for a settings-screen token field, so
the user's ListenBrainz token comes from the web UI (Settings >
ListenBrainz, §8.6), which checks it with ListenBrainz and saves it to
`/var/lib/rpod/listenbrainz_token` (0600), taking effect at once; failing
that, from the environment: `RPOD_LISTENBRAINZ_TOKEN` in the simulator, or
`/etc/rpod/env` (`rpod.service`'s `EnvironmentFile=`) on-device. A saved
token wins over the environment's, and turning it off in the web UI stays
off. No token leaves scrobbling fully inert.

rPod is portable, so "off Wi-Fi" is the normal state, not an occasional
blip — "listen" events are durably queued to a small JSONL file (capped at
2000 entries) and retried roughly once a minute until they succeed,
surviving restarts and power loss in between (`rpod.service`'s
`StateDirectory=rpod` → `/var/lib/rpod/listenbrainz_queue.jsonl`
on-device). `playing_now` pings are deliberately **not** persisted — replaying
a stale "now playing" ping once Wi-Fi returns would be actively misleading,
so those stay purely best-effort/in-memory, dropped if they fail.

A crash or power loss (Phase 7's "pull the battery mid-playback" scenario)
mid-track could otherwise cause a duplicate scrobble on restart, since
"already scrobbled this play" only lived in memory. `src/audio/scrobbler.c`
closes that: it persists the (track, position within it) it last submitted
a listen for to `/var/lib/rpod/scrobbler_state` (`RPOD_SCROBBLER_STATE` in
the simulator), and on startup, if the currently-playing track matches that
record and hasn't gone backwards past a small grace window, treats it as
already handled instead of submitting it again. Comparing track *position*
rather than wall-clock time is what keeps a short track replayed
immediately after finishing from being mistaken for a crash-resume: the
fresh play's position starts back near zero, nowhere near the (already
≥30s, by the scrobble threshold itself) position the previous play was
scrobbled at.

---

## 7. USB-C: charging and host connectivity

> **Deferred.** Music arrives through the web UI (§8.6), so the USB gadget
> and its mount/unmount state machine are not on the path. Kept as the design
> for later; see §9, "Deferred". The charging front end is a PiSugar 2 for
> now: §7.6.

### 7.1 Hardware

One USB-C receptacle feeds two paths:

- **VBUS** → BQ24074 (power path charger) → battery + SYS rail → TPS61030 boost
  → Pi 5 V pin.
- **D+/D−** → the Pi's OTG data pads. Wire to both A6/A7 and B6/B7 on the
  receptacle for cable-flip tolerance.
- **VBUS** → 100k/100k divider → GPIO 22 for host-presence detection.

5.1 kΩ from CC1 to GND and 5.1 kΩ from CC2 to GND — two separate resistors, not
one shared. Without them a compliant USB-C source supplies nothing.

Power path (rather than a plain TP4056) is what allows running from the wall
while charging without cycling the cell. Avoid all-in-one power bank ICs like
the IP5306 — they auto-shut-down at low current draw and will kill the device
whenever it idles.

Feeding the 5 V GPIO pin bypasses the board's polyfuse; add your own
protection.

### 7.2 Gadget configuration

Use **configfs**, not the legacy `g_mass_storage` module. This lets you build a
composite gadget:

| Function | Purpose |
|---|---|
| `mass_storage.0` | The exFAT music partition |
| `ncm.0` | USB ethernet — this is your deploy path |
| `acm.0` | Serial console for debugging |

`system/gadget/setup.sh` builds the gadget tree at boot; the mass storage LUN is
created but left **unbound**.

### 7.3 The state machine that prevents corruption

`mass_storage` gives the host **exclusive block-level access**. If the Pi keeps
that partition mounted while the PC writes to it, the Pi's page cache goes stale
and the filesystem is destroyed. This is not an edge case — it is guaranteed.

Driven off GPIO 22:

**Host connects:**
1. Pause playback, save position.
2. `umount /media/music`.
3. Write the LUN's backing file path to bind it.
4. Display "Connected — do not disconnect".
5. Block all navigation input; only the hold switch responds.

**Host disconnects:**
1. Unbind the LUN.
2. `mount /media/music`.
3. Kick off an incremental library rescan.
4. `mpc update`.
5. Return to the previous screen.

The "do not disconnect" screen is exactly what the original iPod did, and now
you know why.

**Failure handling:** if `umount` returns `EBUSY`, something still holds the
mount. Kill MPD's grip first (`mpc stop`), retry, and only then fall back to
refusing the connection with a visible error. Never force it.

### 7.4 Escape hatch

If the mount/unmount dance proves annoying in daily use — and it will mean
every charge from a PC dismounts the library — drop `mass_storage` and go
NCM-only. Transfer over Samba or SFTP to `rpod.local`. Slower to set up on the
Windows side, but the entire class of failure disappears. Keep the gadget setup
script parameterised so this is a config change, not a rewrite.

### 7.5 Realistic throughput

20–30 MB/s, bounded by dwc2 and the SD card, not USB 2.0's theoretical ceiling.
Set expectations accordingly in the README.

### 7.6 What's fitted now: a PiSugar 2

The front end in §7.1 is still the design for a board of rPod's own. Under
the Zero 2 W today is a **PiSugar 2**: a Li-ion cell, an IP5209 power bank
chip (charger and 5 V boost, I²C 0x75) and an SD3078 RTC (0x32), on GPIO 2/3.
It charges from its own USB port. Its registers come from PiSugar's own
driver (pisugar-power-manager-rs, `pisugar-core/src/ip5209.rs`); rPod
doesn't run PiSugar's server.

- **The level** (`src/power/battery.c`, `gauge.c`). rPod reads the IP5209
  every 2 s on a thread of its own (rpod is in the `i2c` group). It keeps
  no charge count, and its current register is what the Pi draws, not the
  cell's current: it reads the same with USB power in, and the voltage
  doesn't follow it. So the level comes from the cell's voltage alone,
  through PiSugar's discharge curve for this board, averaged over about
  30 s, and it only steps down on battery and up on USB power (unless the
  average moves 5 points the other way). USB power is register 0x78 (not in
  PiSugar's driver; 0x15 with it, 0 without, found by plugging and
  unplugging). While it's in, the charger holds the voltage up by about
  50 mV at once (+52 mV plugging in, −44 mV unplugging), taken off before
  the curve, and then keeps it climbing ahead of the charge (10 mV a
  minute): so while charging, the level climbs from where it was by at most
  a point every 45 s, never past what the voltage says. A first reading
  taken while charging (rPod starting on USB power) has nothing to climb
  from, and reads high until it's unplugged. Charged isn't told apart from
  charging yet: the board's "charging done" LED says the chip knows, but
  which register says so isn't found.
- **Where it shows.** The status bar: Glass has "72%" and a battery like
  iOS's, green while charging, red at 20% or under, with a bolt on USB
  power; Classic has the iPod's 16 levels and its charging animation
  (`re/ipod4/SPEC.md` §5.1). Settings → About. The web UI's title bar and
  About, through the settings socket's `get` (§8.6). Without a PiSugar it
  stays "--%"; `RPOD_BATTERY_FAKE` (e.g. `15`, `80,charging`) fakes one in
  the simulator.
- **Running out.** A HUD notice at 20% and again at 10%. At 3% or under for
  20 s on battery, "Battery Empty", playback paused, and a power-off through
  logind (`system/pisugar/50-rpod-power.rules` lets rpod), before the
  IP5209 cuts out under it. USB power going in first calls it off. The sim
  only logs that it would.
- **Power policy** (`system/pisugar/rpod-pisugar`; `make pisugar-setup`,
  which `pi-setup` runs too). The IP5209 has a power bank's habit, the one
  §7.1 warns about: once the load stays under its threshold (96 mA from the
  factory) for a few seconds, it switches the output off. The Pi draws
  240-470 mA running, but a power-saving sleep needn't, so
  `rpod-pisugar.service` turns it off at boot. A systemd-shutdown hook turns
  it back on as the Pi powers off (PiSugar's own sequence: under 108 mA for
  8 s), so the PiSugar cuts the power a few seconds after the Pi halts,
  rather than keep a halted Pi on until the cell's flat. Its button turns
  it on again. Reboots are left alone.
- **The RTC.** `dtoverlay=i2c-rtc,sd3078` gives `/dev/rtc0`, and the kernel
  sets the clock from it as it registers (`hctosys`, module or not). It
  writes the RTC back once timesyncd has synced, at once and then every 11
  minutes (`CONFIG_RTC_SYSTOHC`). Without it, the clock started each boot
  wherever timesyncd last saved it, hours behind until Wi-Fi came up. An RTC
  that was never set or lost its time reads 2002, and takes the clock with
  it: `rpod-rtc.service`, started by a udev rule when rtc0 appears, puts the
  clock forward to timesyncd's last saved time then.
- **Not used yet:** the PiSugar's button taps (PiSugar's server runs
  scripts on single/double/long taps), the RTC alarm powering the device on
  (it could, for the alarm clock, §8.5), and the SD3078's backup-cell
  charging, which PiSugar's server switches on and off by its flags.
- **Accuracy.** The curve is PiSugar's, made on a Pi Zero, and doesn't fit
  this cell well: 3.94 V to 3.84 V took 38 minutes at 300 mA (190 mAh),
  which it calls 34% of the charge. Tracking reality within 5% (Phase 5's
  acceptance) needs a full discharge recorded on this device and the curve
  in `gauge.c` replaced with it.

---

## 8. UI specification

Landscape, 320×240. Two-pane split like the 4th-gen iPod: menu list on the left
~60%, contextual preview on the right ~40%. Drop to full-width for Now Playing.

### 8.1 Screen graph

```
Main Menu
├── Music
│   ├── Playlists      → track list
│   ├── Artists        → albums → track list
│   ├── Albums         → track list
│   ├── Songs          → track list
│   └── Genres         → artists → …
├── Videos             → the player, full screen (§8.7)
├── Now Playing        (only shown when something is loaded)
├── Settings
│   ├── <AirPods>      (while connected: battery, noise control, settings §6.3)
│   ├── Bluetooth      (scan, pair, connect; AirPods open their settings page)
│   ├── Backlight      (timeout -- brightness needs PWM, §8.3)
│   ├── Haptics        (off / light / strong)
│   ├── Sleep Timer    (pause and sleep in 15-120 min)
│   ├── Auto-Off       (shut down after 15 min-2 h idle, or never; §8.3)
│   ├── Shut Down      (Cancel / Shut Down)
│   └── About          (version, battery, storage, IP)
└── Extras             (§8.5)
    ├── Clock          (the time; Alarm Clock, Sleep Timer)
    ├── Games          (Brick, Music Quiz)
    ├── Home Assistant (the server's devices by kind; §8.8)
    └── Screen Lock    (a four-digit combination)
```

### 8.2 Input mapping

| Input | Action |
|---|---|
| Wheel rotate | Move selection / volume in Now Playing (iOS-style HUD drops in over the status bar; a headset's own volume buttons show it on any screen) |
| Center | Select / cycle Now Playing display mode |
| Menu | Back one level; nothing at the root (so mashing it to back out can't sleep it) |
| Play/Pause | Toggle (on release); held 0.7 s on Now Playing = the Play On list (§6.3b); held 1.5 s anywhere else = pause and sleep -- the iPod's "off" |
| Next | Next track (on release); held 0.5 s = silent scan forward; let go at the end = next track |
| Prev | Restart track, or previous if within 2 s; held 0.5 s = silent scan back |
| Any button, asleep | Wakes, back where it was -- and does nothing else |

Taps vs holds live in `src/input/gestures.c`, shared by the wheel and the
simulator's keyboard. A scan (`src/ui/seek_control.c`) pauses playback and
glides the position -- 8x for the first 2 s held, building to 32x over the
next 3 -- with Now Playing's bar following it at ~30 Hz; letting go seeks
there once and plays on if it was playing. Sleep, for now, is the screen going black,
rendering stopping and the backlight switched off; Phase 5's low-power sleep
builds on it. Not FBIOBLANK: fbtft sends its DISPOFF/DISPON unsynchronised
with a frame push in flight, and it left the panel dark (see
`src/ui/lvgl_port.c`). Rotation is ignored while asleep. Settings > Sleep
Timer sleeps the same way once its time is up; sleeping by hand cancels it.
The backlight timer's lighter version of this is in §8.3. Lists stop at
their ends rather than wrapping around, so a flick that overshoots the top
stays there.

**Scroll acceleration is what makes this feel right or wrong.** Track angular
velocity across the last ~150 ms and apply a non-linear multiplier so a fast
flick jumps many rows. Without it, a 2000-song list is unusable. Budget real
tuning time for this; it is the single highest-leverage feel detail in the
project. Make the curve parameters live-tunable over the ACM serial console so
you can adjust without a rebuild. (As built, they're live-tunable from the
web UI instead -- Settings > Click Wheel (§8.6), saved to
`/var/lib/rpod/wheel` -- over defaults that the `RPOD_WHEEL_*` variables in
`/etc/rpod/env` can still change; `src/input/wheel_tuning.h` lists them.)

Add the alphabet-scrub overlay for long lists: past a velocity threshold, show
a large centred letter and jump by first-letter index. As built: in a list of
25 or more alphabetical rows (Artists, Albums, Genres, a genre's artists, the
flat Songs list), turning faster than `RPOD_WHEEL_SCRUB_V` (default: where the
acceleration curve tops out) brings up the current letter, and from then on
every `RPOD_WHEEL_SCRUB_TICKS` positions (default 16, six letters a turn) jumps
a letter, at any speed, so it can be slowed down to land on one. Lifting the
finger, or resting 0.6 s, goes back to rows. The simulator's stand-in is
Shift+Left/Right. Those lists are sorted the iPod's way (`src/ui/alpha_sort.c`)
rather than MPD's byte order: case and Latin accents folded, a leading "The "
and punctuation skipped, and anything not starting with a letter under '#',
after Z. Songs sort by title.

### 8.3 Rendering discipline

- No full-screen redraws outside screen transitions.
- Album art decoded once, cached at display size, never re-decoded on scroll.
- Now Playing progress updates at 1 Hz, not per-frame.
- The backlight turns off on an idle timer (Settings > Backlight: 10 s to
  2 min, or always on; 30 s by default, `src/ui/backlight.c`). It's the
  same dark screen as sleep, but lighter: touching or turning the wheel
  wakes it, and that turn doesn't move the selection. Center and Menu only
  wake it too, so nothing is selected unseen. Play/Pause, Next and Prev wake
  it *and* act, the way an iPod's do in a pocket. Off rather than faded, and
  no brightness setting yet: the backlight is a plain GPIO under fbtft.
  Dimming needs hardware PWM on GPIO 13, which the 3B's headphone jack and
  pigpio's DMA pacing both want (§4.5).
- Off is a real power-off (`src/ui/shutdown.c`): Settings > Shut Down asks
  first (Cancel, then Shut Down, as the iPod's Reset All Settings did), and
  Settings > Auto-Off shuts down by itself after 15 min to 2 h with nothing
  happening on battery, or never; 30 min by default. On USB power it waits:
  there's no battery to save. Nothing happening is no wheel or buttons,
  nothing playing, and the web UI not in use: rpod-web sends
  `active` over the settings socket every minute while a request's under
  way or just done (its own state polling aside, so a tab left open doesn't
  count) or a video's converting. Not while an alarm is set, which couldn't
  ring with rPod off. logind powers the Pi down, the PiSugar cuts the power
  after (§7.6), and its button brings rPod back -- a full boot, so sleep
  (§8.2) stays the quick "off". "Auto-Off", not "Auto Shut Down": the
  Classic title bar fits about eleven Chicago characters.

### 8.4 Themes

Settings > Theme picks the look, persisted like the backlight timeout
(`/var/lib/rpod/theme`):

- **Glass** -- rPod's own dark "liquid glass" UI, the default.
- **Classic** -- the 4th-generation iPod's UI, rebuilt from its firmware
  (3.1.1, reverse-engineered statically: `re/ipod4/SPEC.md`). Its 160×128
  screen laid out at 2x: the title bar (play state, the screen's title,
  battery, a rule beneath), menu rows in Chicago with the selected row
  inverted and the iPod's own arrow, its scrollbar, and its Now Playing --
  "3 of 12", repeat and shuffle, three centred lines, the rounded progress
  bar, which the volume bar replaces while the wheel turns it. The panel is
  16 lines short of 2x, so rows are 33px rather than 36 (six still fit)
  and Now Playing gives up 8 iPod pixels, per SPEC.md §8.
- **Classic Dark** -- the same, its four grays turned around.

A screen's title is the label of the row that opened it, as on the iPod
(`screen_stack.h`); builds name their own where they can be reached
another way. The Classic themes branch layout, not just colour, in the
status bar, list rows, Now Playing and the Songs list -- everything else
just takes the theme's colours and fonts. Switching rebuilds every screen on
the stack in place, keeping each one's highlighted row.

The iPod's fonts and bitmaps are Apple's, so they're never committed (§12).
Only the extractor is (`re/ipod4/tools/`): `make ipod-assets` downloads
firmware 3.1.1 from Apple (hash-pinned) and runs it, and the Makefile
compiles its LVGL output in when `IPOD_ASSETS` (default `re/ipod4/out/lvgl`)
holds it. Without it Classic keeps its layout but draws with Montserrat and
LVGL's symbols, and the build prints a note saying so. Glass must stay
pixel-identical across Classic work: compare it headlessly (CLAUDE.md's UI
notes) against the previous build.

### 8.5 Extras

The 4th-gen iPod's Extras (re/ipod4/SPEC.md §6), less the organiser
(Contacts, Calendar, Notes) and the photo and voice-memo imports, plus the
later iPods' Screen Lock:

- **Clock** -- the time and date, big (the iPod's own clock numerals in the
  classic themes, `src/ui/big_digits.c`), over Alarm Clock and Sleep Timer
  (Settings' own screen). The time is the system's, like the status bar's:
  the Pi has no real-time clock, so it's right once NTP has set it.
- **Alarm Clock** (`src/ui/alarm.c`) -- on or off, a time, and what it
  plays: Shuffle Songs (the whole library, queued and shuffled
  server-side) or a stored playlist, shuffled. The time is set the iPod's
  way, hours then minutes with an arrow above and below
  (`src/ui/spinner.c`), and setting it switches the alarm on. It goes off
  every day at its time: the music starts at a tenth of the volume and
  fades up over ~30 s -- a volume-memory duck (§6.3) stepped back up, so
  the quiet start is never remembered as the device's level, and turning
  the volume during it ends the fade there. The screen wakes from either
  sleep with the alarm's card over whatever's up: Snooze (9 minutes,
  paused meanwhile) or Stop (paused). Menu closes the card and leaves the
  music playing.
- **Brick** -- the iPod's breakout. The rules and physics are
  `src/games/brick.c` (unit-tested); `src/ui/screens/brick_screen.c` draws
  them. The wheel steers the paddle in fine rotation (`src/input/input.h`):
  every wheel position is a step, with no acceleration or alphabet scrub,
  and the simulator repeats a held arrow key. The game pauses itself when
  it's out of sight -- the screen dark, or a card over it.
- **Music Quiz** -- ten rounds: a song from the library plays from somewhere
  in its middle, and you pick its title from five before ten seconds run
  out. Quicker answers score more, and the result reads the iPod's way,
  "Score: 640 (7/10)". Each round's song is added to the end of the queue
  and played by its id, so the queue stays as it was, and leaving the quiz
  puts back the song that was playing, at its position, playing or paused
  -- unless something else took playback over meanwhile (Next, the alarm),
  which then stands. The scrobbler is suspended throughout (a snippet from
  past the middle would otherwise count as a listen), and the glass status
  bar reads "Music Quiz" instead of the answer.
- **Screen Lock** -- a four-digit combination, entered twice to set it.
  Locked, Menu can't leave the lock screen (`rpod_screen_stack_set_locked()`)
  while Play/Pause, Next and Prev still work and cards (the alarm, the
  AirPods) still open over it; the classic title bar shows the iPod's hold
  icon. It holds through sleep, a theme change and a restart. The way back
  in from a forgotten combination is deleting `/var/lib/rpod/lock` over SSH.

Their state is in `/var/lib/rpod`: `alarm`, `scores` (Brick's and Music
Quiz's bests) and `lock`, each overridable like the rest
(`RPOD_ALARM_STATE`, `RPOD_SCORES_STATE`, `RPOD_LOCK_STATE`).

### 8.6 Web UI

`http://rpod.local/`, served by the device whenever it's on the network:
music uploads, the library, and the settings a click wheel can't reach.
It's dressed in whatever theme the device has (§8.4), and follows it live:
Glass with its translucent panels (here with the real backdrop blur the Pi
can't afford, over the cover art), or the iPod at 2x -- its title bar and
rule, rows of Chicago with the selection inverted, its arrow, scrollbar
and rounded sliders. The screens drill down like the device's menus, each
titled by the row that opened it, and the keyboard drives them like the
wheel: the arrows move the selection, Enter is Center, Escape is Menu,
Space is Play/Pause, N and P are Next and Prev.

- **Upload Music**: drop songs or whole album folders anywhere on the page
  (or pick them), and they go into the folder in view, two at a time with
  progress, the folders' structure kept. Names are made safe for the music
  partition's exFAT; anything that isn't music MPD plays (or a cover
  image, cue sheet or lyrics) is left out; a file already there is
  skipped unless Replace is on. MPD updates the folder afterwards, and the
  page says how many songs arrived.
- **Music**: the music directory's folders and files, with MPD's tags --
  filter, sort by name, artist, album (grouped), date added or size; play a
  song (its album queued from there, as on the device), rename, delete,
  make folders, update the library.
- **Videos**: upload them (or drop them on any screen: a video goes here
  wherever it's dropped), see each one's conversion go, then play it on
  rPod, watch it in the browser, rename or delete it (§8.7).
- **Now Playing**: the cover (Glass) or the iPod's three lines (Classic),
  progress to click and seek, transport, volume, shuffle and repeat -- a
  video's poster while one plays on the device.
- **Settings**: everything the device's Settings has -- Theme, Backlight,
  Sleep Timer, Alarm Clock -- plus MPD's Crossfade, Sound
  Check (ReplayGain) and volume, **Video Sync** (§8.7), **Click Wheel** (the acceleration curve of
  §8.2, drawn, with a slider per number; the wheel follows as a slider
  moves, which is what "live-tunable" in §8.2 became),
  **ListenBrainz** (the token §6.5 had no way to type: checked against
  ListenBrainz, then saved), and **Home Assistant** (the server's address
  and access token, which §8.8 has no keyboard for).
- **About**: the iPod's About, with what the Pi knows on top -- library
  counts and size, addresses, uptime, memory, temperature, versions.

It's `rpod-web` (`web/`), a process of its own like the click wheel
daemon, so a slow upload or a bug can't stall or crash the UI, and its own
systemd unit (`system/systemd/rpod-web.service`, port 80 through
`CAP_NET_BIND_SERVICE`, niced). It's C on libmpdclient and libcurl, both
already on the Pi, with just enough HTTP/1.1 of its own (every response
closes its connection; bodies by Content-Length only). A thread per
connection, eight at most. The page is plain HTML, CSS and JavaScript in
`web/static/`, served as files: no build step, no framework, nothing
fetched from the internet.

- **The library** it reads and writes directly, under MPD's
  `music_directory` (`web/library.c`). Every path from the browser is
  checked component by component (no `..`, nothing hidden, exFAT's rules)
  and walked with `openat(O_NOFOLLOW)`, so neither `..` nor a symlink leads
  out of it. An upload streams to a hidden temp file in its folder --
  never through memory, and MPD never scans half a song -- with
  `sync_file_range()` and `POSIX_FADV_DONTNEED` every 8 MB so an album's
  worth of dirty pages doesn't crowd 512 MB (page-cache pressure once
  stalled the wheel's DMA, §4.5), then `fdatasync()` and a no-replace
  rename into place. 64 MB of the partition is always left free.
- **Playback and MPD's options** go to MPD. Cover art is read straight out
  of the FLAC (`src/audio/embedded_art.c`) or a cover image beside it,
  never MPD's `readpicture`, which holds every client up (§6.4).
- **rPod's own settings** go through rPod (`src/ui/control.c`): a Unix
  socket in `/run/rpod-ui` (rpod.service's `RuntimeDirectory`), polled from
  an lv_timer like the wheel's, one line each way. A change takes the same
  setter a Settings screen uses, so it lands at once and persists as
  usual, and a theme change rebuilds the device's screens. rpod-web never
  writes rPod's state files itself -- it only reads the theme file, to
  dress the page without a round trip. With rPod not running, those
  settings say so; everything else still works.

**Who may use it.** Nobody outside the network, which is all rPod's own
Wi-Fi gives it. Within it: the Host header must be this machine (an IP,
`localhost`, the hostname, or it under `.local`, `.lan`, `.home`,
`.internal`, `.home.arpa`, or `RPOD_WEB_HOSTS`), so a web page elsewhere
can't reach the API by pointing its own DNS name at rPod's address. Every
request that changes anything must carry `X-Rpod: 1`, which a page on
another origin can't add without a CORS preflight that's never granted
(and an `Origin`, if sent, must match). With `RPOD_WEB_PASSWORD` in
`/etc/rpod/env`, every request needs it too (HTTP Basic, any user name).
The Screen Lock's combination isn't reachable from here on purpose.

**Classic's font and icons.** The page uses the iPod's own Chicago, Pixo
and bitmaps when `make ipod-assets` has made them: the extractor
(`re/ipod4/tools/ipod4re/webfont.py`) turns the bitmap fonts into TrueType
(each pixel a square of outline, 1/16 em, so 32px is exactly 2x) and the
bitmaps into SVG masks, into `re/ipod4/out/web/`; `make deploy-web` copies
them beside the page, which finds them at `/ipod/`. Like the LVGL fonts
they're Apple's and never committed (§12), and the page works without
them: a bold system font, and look-alike icons drawn for it.

Not done: the library writes assume the music partition stays mounted --
Phase 5's USB mass-storage state machine (§7.3) will have to make uploads
refuse politely while the host has it. And the cover cache (§6.4) isn't
told about re-tagged art that's uploaded over an album it already has.

### 8.7 Videos

Main Menu > Videos, as on the iPod with video: upload one from the web UI
(§8.6), and it plays full screen with its sound wherever the music's goes.

**Conversion, the iPod's way.** iTunes made an "iPod version" of a movie
before it was synced; rPod makes its own when the upload arrives.
`rpod-convert` (`web/rpod_convert.c`, C on FFmpeg's libraries, which MPD
has already put on the Pi) turns anything FFmpeg reads into one plain
kind: the picture fitted to 320×240 (aspect kept, square pixels, a phone's
rotation applied), 30 fps at most (60 halves to 30, 50 to 25), H.264 Main,
with stereo AAC at 44.1 kHz, in an MP4 with its index first. A video
without sound gets a silent track, since MPD plays the sound and its
position is the clock. It also writes a poster: a frame a tenth of the way
in (30 s at most), as a JPEG. A minute comes to about 4 MB.

`rpod-web` runs it after an upload (`web/videos.c`), one at a time, as a
child under the idle scheduler for both CPU and disk, so it only ever gets
what playback and the UI leave over -- and with an OOM score of 1000, so a
source too big for 512 MB costs the conversion, not rPod or MPD. The
upload arrives whole first, streamed to a hidden file the way a song is
(§8.6), and the original goes once the conversion's done. A job outlasts
a restart (`.rpod-source-<n>` and its `.job` file, which holds what it'll
be called), so a conversion cut short starts over when rpod-web does. The
page shows each one's progress and time left, and can stop one. Decoding
the source is most of the work: going 1080p down to 320 wide hides any
blocking, so for a source at least twice the output's size the deblocking
filter is skipped, which saves about a fifth of it. Measured on the dev
machine: a minute of 1080p H.264 in 5 s. Still to measure on the Pi;
expect around real time for 1080p H.264, slower for HEVC, and 4K HEVC is
too much to be worth it.

**Where they live.** Videos sit in their own folder, `/media/videos`
(`RPOD_VIDEO_DIR`, the same for rPod and rpod-web), not in MPD's
`music_directory`: MPD would take every video there for a song, and an
ignored one (`.mpdignore`) can't be played at all, since MPD maps a path
inside its music directory back to its database. Outside it, MPD plays a
video's sound given its absolute path -- which it allows only clients on
its local socket, as rPod is, and only for files they could read
themselves. Each video is a file in the folder, with its poster at
`.posters/<name>.jpg`; anything else FFmpeg reads that's copied in by hand
plays too (scaled to fit as it plays, at whatever that costs), just
without a poster or a known length.

**Playing one** (`src/ui/screens/video_screens.c`) borrows playback the way
Music Quiz does (§8.5): the video is added to the end of the queue and
played by itself, and leaving it -- Menu, its end, or running a scan off
it -- puts back the song that was playing, where it was, paused. If
something else takes playback over meanwhile (the alarm, Next in the web
UI), that stands and the player goes. The scrobbler is suspended
throughout, and the status bar hidden.

- The wheel is the volume, as on Now Playing. Center brings up the title
  and progress bar with the wheel scrubbing instead -- the iPod's way --
  until it's left alone for 3 s; paused, the bar stays up.
- Play/Pause pauses (MPD's, so AirPods taken out pause it too, §6.3).
- Next goes to the next video in the list, back to it after the last; Prev
  to the start, or to the one before within 2 s of it. Held, they scan as
  for a song (§8.2), with the picture following, a keyframe at a time.
- The backlight timer holds off while it plays (§8.3); the sleep timer and
  Play/Pause held still pause and sleep.
- The web UI's Play on rPod puts it up at once, from any sleep -- but never
  over the Screen Lock, which it would get round.

The picture is decoded on a thread of its own (`src/video/video_decoder.c`)
into four RGB565 frames ahead, and an lv_image shows each when its time
comes, with an `lv_refr_now()` so it goes out to the panel at once rather
than at LVGL's next 33 ms refresh -- 30 fps against that would come out
unevenly. The panel's 25.4 ms push (§5.3) makes 30 fps about the most it
can show; a frame decoded too late is dropped before it's converted. At
320×180 a frame is a few ms of H.264 on one A53. Its time is MPD's
position in the sound: polled every 200 ms, and run as a clock in between
(`src/video/media_clock.c`), eased toward each answer so MPD's ~20 ms
chunks don't jitter the frames, and taken at once when it jumps (a seek
from the web UI, say), which moves the picture there too.

**Sync.** MPD's position is the last of the sound it's handed an output,
which then holds some before it's heard, so the picture runs that much
behind it (`src/video/av_sync.c`): on the headphone jack, ALSA's ring
buffer -- half a second by MPD's default, read off the kernel
(`/proc/asound/.../status`) while it plays; on Bluetooth, PipeWire's
graph and the headset's own buffer, guessed at 250 ms. On top of either
is the user's own trim, Settings > Playback > Video Sync in the web UI
(±500 ms, saved to `/var/lib/rpod/video`, `RPOD_VIDEO_STATE`).

Still to verify on hardware: the conversion's speed and memory on the Pi
for 1080p H.264 and HEVC; that the Pi's MPD has its ffmpeg decoder (`make
deploy-web` says if not); how far off the Bluetooth guess is (BlueZ's
`MediaTransport1.Delay`, which AirPods report, could replace it); and that
30 fps holds with the push and the decode together.

### 8.8 Home Assistant

Extras > Home Assistant controls a Home Assistant server from the wheel. It
lists what the server has by kind -- Lights, Switches, Fans, Covers, Locks,
Climate, Scenes, Scripts, Automations, Buttons and (read-only) Sensors,
the kinds with nothing in them left out -- and Center works a device:

- A **switch, fan, automation or input boolean** toggles; a **cover**
  opens or closes; a **lock** locks or unlocks; a **scene** is activated, a
  **script** run, a **button** pressed.
- A **light with a brightness** and a **thermostat** open a page of their
  own, as their row's chevron says: a light has Turn On/Off and Brightness,
  a thermostat its Target, the room's temperature and its mode. Brightness
  (0-100, 0 turns it off) and the target temperature (whole degrees) are
  set with the number spinner Alarm Clock's time uses (`src/ui/spinner.c`):
  turn the wheel, Center sends it, Menu leaves it as it was.
- A row shows the device's state on its right (a light's brightness, a
  sensor's reading with its unit, a thermostat's target), and what's
  unavailable reads as such, since Center does nothing on it.

**Setting it up.** The wheel has no keyboard, so the server's address and a
long-lived access token (Home Assistant's profile > Security) go in through
the web UI's Settings > Home Assistant (`ha_url` and `ha_token` on the
settings socket, `src/ui/control.c`), which then shows how rPod's getting
on with it: connected to *Home*, the token rejected, no route to the
address. They're saved in `/var/lib/rpod/homeassistant` (0600; the token is
a credential and `get` never returns it), overridable with
`RPOD_HA_STATE`. `homeassistant.local` needs mDNS, which is flaky on this
network (CLAUDE.md): the server's IP works anywhere. A self-signed
`https://` certificate is refused rather than trusted blindly; plain
`http://` on the LAN (Home Assistant's own default, port 8123) works as is.

**How it talks** (`src/ha/ha.c`). Home Assistant's REST API over libcurl:
`GET /api/states` for everything, `POST /api/services/<domain>/<service>`
for an action, then `GET /api/states/<id>` to see how it went -- a device
reports its new state a moment after the call, so that waits 350 ms. All
of it on one worker thread, because a server that's off or out of Wi-Fi
range takes seconds to time out and the LVGL thread never waits; screens
read the last snapshot and poll `rpod_ha_version()` to redraw (a screen
rebuilds only if what it shows changed, so a sensor ticking over doesn't
rebuild a list of lights). A press changes the cached state at once, and
the server's reply, or a refresh, corrects it: an action that failed
reverts, with the reason in a HUD message.

It polls (every 5 s, while one of these screens is in front) instead of
holding a WebSocket open: an idle rPod costs the server nothing and the
Zero 2 W no wake-ups, and nothing touches the network at boot, so none of
§3's timing changes. Redirects aren't followed, so the token goes only to
the address it was given; an address with credentials, a query or another
scheme is refused, and an entity id is checked (`light.kitchen`'s
characters, 63 at most) before it goes into a URL or a request body.
Entities with longer ids are left out, as a list row's key holds 63 bytes.

`tools/fake-ha.py` is a Home Assistant that speaks that API over a score of
devices whose state the services really change, for the sim and the web UI
(`FAKE_HA_FAIL=1` and `FAKE_HA_DELAY=2` make it break and drag). `make test`
covers the JSON reader (truncated replies included), the address and id
rules, the state parser and what Center does per kind.

Not done: areas and devices (the REST API doesn't carry them -- grouping
needs the WebSocket's registries, or templates), media players (the iPod
is one), fractional target temperatures, and colour. Not tried yet on the
Zero 2 W itself: the REST reply for a big installation is parsed whole
(capped at 8 MB), which wants a look at memory on the device.

---

## 9. Phased build plan

Each phase ends with a working, demonstrable device state. Do not batch phases.

### Phase 0 — Bring-up
Flash Bookworm Lite 64-bit. SSH over Wi-Fi. Confirm SPI, I²C, and I²S enumerate.
Verify Pi model and kernel version. Set up the Makefile deploy target:
`rsync` to `rpod.local` + `systemctl restart rpod`.
**Accept:** `make deploy-run` puts a hello-world binary on the device in under
5 seconds, end to end.

### Phase 1 — Display
Get the panel lit via §5.2 (fbtft) first. Fill red, green, blue full-screen.
Confirm no offsets, correct orientation, no colour inversion. Then LVGL with a
"Hello" label and a spinner.
**Accept:** LVGL renders and animates at a measured ≥20 fps.

### Phase 2 — Click wheel
Build `tools/wheel-sniff.c`. Derive the real bit map per §4.3 and document it.
Then write `rpod-wheel.c` and a test client that prints normalised events.
**Accept:** every button reports press and release exactly once; a full slow
rotation reports monotonic deltas summing correctly with no wraparound glitches
and no dropped packets over 60 seconds of continuous scrolling.

### Phase 3 — Audio (deferred)
Audio goes out over Bluetooth (Phase 6), with the Pi 3B's headphone jack on
the dev board. The wired DAC is moved to "Deferred" below.

### Phase 4 — UI integration
Wire the wheel into LVGL as an encoder indev. Build the screen graph. Connect
to MPD. Implement scroll acceleration and tune it.
**Accept:** navigate a 1000+ track library and start playback using only the
click wheel, with no SSH session open.

### Phase 5 — Power and USB (deferred)
Moved to "Deferred" below. Uploads go through the web UI. The battery
indicator and the power-off when it runs out are done, on a PiSugar 2
(§7.6).

### Phase 6 — Bluetooth (optional)
BlueZ + PipeWire, pairing UI, output switching. AirPods extras over AAP (the
LibrePods spike) per §6.3.
**Accept:** pair AirPods from the device UI and switch output mid-track without
restarting MPD.

### Phase 7 — Hardening
Read-only rootfs with an overlayfs for config and rPod's state
(`/var/lib/rpod`, the cover cache in `/var/cache/rpod`). Buildroot
image if boot time still isn't acceptable. Watchdog. Crash recovery that
restores playback position.
**Accept:** pull the battery mid-playback 20 times; the device boots cleanly
every time and resumes within 2 seconds of where it stopped.

### Deferred

Dropped from the critical path on 2026-10-16: audio is Bluetooth and music
arrives through the web UI. The specs stay (§6.2's DAC output, §7), to pick up
if the hardware is ever wired. Nothing else depends on them.

#### Was Phase 3 — Audio
Wire the PCM5102A. `speaker-test` through the DAC. Install and configure MPD.
Play a FLAC via `mpc`. Confirm bit-perfect: play a 24/96 file, check
`/proc/asound/card0/pcm0p/sub0/hw_params` reports 96000 and S24, not resampled.
**Accept:** 24/96 FLAC plays with no resampling and no dropouts under simulated
UI load.

#### Was Phase 5 — Power and USB
Build the charging front end. Fuel gauge over I²C, battery indicator in the UI.
Composite gadget + the §7.3 state machine. Sleep/wake path.
**Accept:** plug into a PC — mounts as a drive, library dismounts cleanly;
unplug — library remounts and rescans; copy a new album across and it appears
in the UI without a reboot. Battery percentage tracks reality within 5%.

---

## 10. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Click wheel is not 4th gen | Medium | Sniffer in Phase 2 detects it immediately; §4.4 |
| pigpio drops edges under audio load | Medium | 1 µs sample rate; RP2040 fallback per §4.6 |
| pigpio's DMA pacing collides with I²S or PWM backlight | High | PCM on the 3B, `RPOD_WHEEL_PACING=pwm` once the DAC is in; RP2040 per §4.6 frees both (§4.5) |
| 512 MB RAM insufficient | Low | zram; cap LVGL buffers; MPD DB on disk not memory |
| SPI display too slow for scroll | Low | Partial redraw is mandatory, not optional |
| Boot time unacceptable | High | Never fully power down — sleep instead. Buildroot as plan B |
| AAP shifts with AirPods firmware | Medium | Isolated in `audio/airpods.c`; rows only for reported settings; `RPOD_AIRPODS_DEBUG` logs packets |
| exFAT corruption from bad unmount | Medium | §7.3 state machine; NCM-only fallback in §7.4 |
| Boost converter browns out on Wi-Fi TX | Medium | Size for 1.5 A peak; bulk cap at the Pi's 5 V pin |
| The PiSugar's IP5209 switches off at light load | High | Off while running, back on at poweroff only (§7.6) |
| Web UI used by someone else on the network | Medium | Host check, `X-Rpod` header, optional `RPOD_WEB_PASSWORD`; paths confined to the music directory (§8.6) |
| Converting a big video starves or OOMs the Pi | Medium | One at a time, idle CPU and I/O scheduling, OOM score 1000 so the converter goes first; no deblocking when shrinking (§8.7) |

---

## 11. Explicitly out of scope for v1

Keep these out of the critical path. Note them, build the structure to allow
them, implement none of them until Phase 7 passes.

- Streaming service clients
- Podcast support with position tracking
- Voice memos / recording
- Wi-Fi sync from a home server
- Equaliser

---

## 12. Licensing

rPod's own code and docs are **CC BY-NC 4.0** (`LICENSE`; summary in
`README.md`). Vendored and derived third-party parts keep their own licenses.

The click wheel decoding approach derives from
`dupontgu/retro-ipod-spotify-client`, **Apache-2.0**. If any of that code is
carried over rather than reimplemented from the documented protocol, retain the
license header and attribution, and note it in `README.md`. LVGL is MIT.
Reimplementing from the protocol description in §4 avoids the question
entirely, and is recommended for other reasons anyway.

The Classic themes' fonts (Chicago, Pixo) and bitmaps are Apple's, extracted
from iPod firmware for interoperability: they stay out of the repository and
are generated locally (§8.4). Their layout -- positions, sizes, colours,
drawing rules -- is recovered from the firmware and reimplemented in
`src/ui/classic.c` and the screens.

Note that the `-Pod` suffix is Apple trademark territory. Fine for a personal
project on GitHub; reconsider the name before selling boards or kits.

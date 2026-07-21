# rPod — Build & Implementation Plan

A Raspberry Pi Zero 2 W music player using an original 4th-gen iPod click wheel,
a Waveshare 2" ST7789V LCD, an I²S DAC for bit-perfect lossless output, and a
single USB-C port for charging plus host connectivity.

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
| BQ24074 | Charger + power path | Power path is mandatory — see §7 |
| TPS61030 or TPS613222 | Boost 3.7 V → 5 V | Size for 1.5 A peak |
| MAX17048 | Fuel gauge | I²C, gives real % rather than a voltage guess |
| Li-ion cell, 3000–5000 mAh | Power | Single cell |
| USB-C receptacle | Charge + data | 5.1 kΩ on each of CC1/CC2 to GND, separately |
| ERM/LRA vibration motor + driver | Haptics | Click wheel scroll feedback |

### 1.2 GPIO pin map

This map is conflict-checked. Do not reassign without re-checking peripheral
overlap (SPI0, PCM/I²S, and PWM all have fixed pin options).

| GPIO | Function | Peripheral |
|---|---|---|
| 2 | I²C SDA | Fuel gauge |
| 3 | I²C SCL | Fuel gauge |
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
│   └── wiring.md
├── src/
│   ├── main.c                # event loop, app entry
│   ├── input/
│   │   ├── wheel.c/.h        # click wheel decoder (runs as root helper)
│   │   └── events.c/.h       # normalised input event types
│   ├── ui/
│   │   ├── screens/          # one file per screen
│   │   ├── theme.c/.h        # fonts, colours, metrics
│   │   └── lvgl_port.c       # LVGL init + display/input driver binding
│   ├── audio/
│   │   ├── mpd_client.c/.h   # libmpdclient wrapper
│   │   └── outputs.c/.h      # DAC vs Bluetooth output switching
│   ├── library/
│   │   ├── db.c/.h           # SQLite tag index
│   │   └── scan.c/.h         # filesystem walk + taglib extraction
│   ├── power/
│   │   ├── battery.c/.h      # MAX17048 over I²C
│   │   └── usb_gadget.c/.h   # VBUS state machine, LUN bind/unbind
│   └── util/
├── daemon/
│   └── rpod-wheel.c          # standalone root process, pigpio + Unix socket
├── system/
│   ├── config.txt.d/         # dtoverlay fragments
│   ├── systemd/              # unit files
│   ├── udev/
│   └── gadget/               # configfs setup script
├── tools/
│   ├── wheel-sniff.c         # raw 32-bit packet logger for §4.3
│   └── sim/                  # SDL harness to run UI on the dev machine
└── third_party/
    └── lvgl/                 # submodule, v9.x
```

---

## 3. System base

**OS:** Raspberry Pi OS Bookworm **Lite, 64-bit**. No desktop.

**`/boot/firmware/config.txt` additions:**

```ini
dtparam=spi=on
dtparam=i2c_arm=on
dtparam=audio=off
dtoverlay=hifiberry-dac
dtoverlay=dwc2,dr_mode=peripheral
gpu_mem=32
disable_splash=1
boot_delay=0
```

`hifiberry-dac` is the correct overlay for a PCM5102A — it is a plain I²S
slave with no control interface, same as the HiFiBerry DAC.

**`cmdline.txt`:** append `modules-load=dwc2` and remove `console=serial0` if
you want the UART pins back. Add `quiet` and `logo.nologo`.

**Partition layout on the SD card:**

| Partition | FS | Purpose |
|---|---|---|
| p1 | FAT32 | boot firmware |
| p2 | ext4 | rootfs |
| p3 | exFAT | **music library — exposed over USB, see §7** |

The music partition must be separate. Do not put music on the rootfs.

**Packages:**

```
build-essential pkg-config git
libpigpio-dev
libdrm-dev libmpdclient-dev libsqlite3-dev libtag1-dev
mpd mpc
exfatprogs
```

**Boot time:** the target is under 8 seconds to first UI frame. Disable
`NetworkManager-wait-online`, `systemd-networkd-wait-online`, `apt-daily`,
`man-db`, and `triggerhappy`. If that still isn't fast enough, the fallback is
never fully powering down — screen off and CPU idle instead of shutdown. Build
the sleep path either way.

Add zram swap (`zram-tools`, ~256 MB, lz4). 512 MB is tight once LVGL's frame
buffers and MPD's cache are resident.

---

## 4. Click wheel

### 4.1 What it is

The 4th-gen click wheel is a self-contained capacitive sensor that emits a
continuous synchronous serial stream on two lines: `CLOCK` and `DATA`. It is a
3.3 V part. Both lines need pull-ups (internal pull-ups on the Pi are
sufficient). There is no command channel — you only listen.

Packets are 32 bits, emitted repeatedly while the wheel is touched or a button
is held.

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

Wheel position is an absolute 8-bit value around the ring, not a delta. Compute
deltas yourself and handle wraparound: a jump from 250 to 5 is forward motion,
not a 245-step reverse.

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

- Uses `pigpio` with `gpioSetAlertFunc()` on both CLOCK and DATA.
- **Set the sample rate to 1 µs** via `gpioCfgClock(1, 1, 0)` before
  `gpioInitialise()`. The 5 µs default will drop edges. This raises idle CPU
  noticeably — measure it, and consider 2 µs if 1 µs is too hungry.
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
dtoverlay=fbtft,spi0-0,st7789v,width=240,height=320,rotate=90,reset_pin=27,dc_pin=24,led_pin=13,speed=62500000
```

`width`/`height` here describe the panel's native (portrait) raster; `rotate`
is what turns it into a 320×240 logical framebuffer. 90 vs 270 depends on
which edge of the glass the ribbon connector comes off — confirm against
real text on the panel (a solid colour fill can't tell you the read
direction) and flip it if "rPod" comes out upside-down or sideways.

Start here if §5.1 fights you. Migrating later is a contained change confined
to `lvgl_port.c`.

### 5.3 Performance envelope

240 × 320 × 16 bpp = 153,600 bytes per full frame. At 62.5 MHz SPI that is
~30 fps theoretical, ~25 fps real with DMA overhead. This is fine **provided
you never do full-frame redraws**. Configure LVGL for partial rendering with
two ~40-line draw buffers and let it dirty-rect. Scrolling a list should be
touching a fraction of the panel per frame.

**Measured on real hardware** (fbtft, `rotate=90`): the SPI core didn't
honour the requested 62.5 MHz — `dmesg` reports it settled at 32 MHz — and
the label+spinner scene renders at a measured 30 fps, comfortably clearing
the Phase 1 accept bar. Two hardware-specific gotchas worth knowing before
touching `src/ui/lv_conf.h` again:

- `LV_MEM_SIZE` needs real headroom above the draw buffers. At 64 KB, the
  two 320-wide partial buffers (51,200 bytes) left too little for LVGL's
  own widget/font/layer allocations, and `LV_USE_ASSERT_MALLOC`'s handler
  (`while(1);`) turned a failed allocation into a silent hang — 100% CPU,
  no crash, no log output (logging's off), screen frozen. 256 KB fixed it;
  512 MB total RAM makes this a non-issue either way, so don't be stingy.
- `LV_LINUX_FBDEV_MMAP` must stay `0` (pwrite) on this panel with `rotate=`
  active. mmap'd writes from LVGL's long-running process never reach the
  panel — confirmed the driver mechanism itself is otherwise sound (a
  throwaway test program doing long-lived, rapid, repeated mmap writes at
  LVGL's redraw cadence worked every time), so this looks like a narrow
  staging-driver bug specific to LVGL's exact access pattern, not something
  worth chasing further. Don't flip this back to 1 without re-verifying on
  the actual panel, not just fps/CPU numbers — a wedged flush still leaves
  the process looking "active."
- Under heavy rapid testing (many opens/mmaps/writes across multiple
  processes in a short window, no reboot in between) the driver's internal
  state can wedge — writes stop reaching the panel with zero kernel-side
  error, everything still "works" from software's point of view. A reboot
  clears it. Don't chase this as a config bug if it happens; just reboot
  and re-test.

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

**LibrePods** adds the Apple-specific extras (in-ear detection, noise-control
mode switching, battery levels) by speaking the Apple Accessory Protocol over a
dedicated L2CAP channel. Porting it is a **separate spike with its own
timebox**, not a line item inside another phase. Before committing time to it:

1. Confirm plain A2DP works and is good enough.
2. Read the current LibrePods source and verify the Linux path is maintained.
3. Only then decide.

Be honest in scoping: everything over Bluetooth is lossy regardless. The
lossless story is the headphone jack. Do not let AirPods integration drive
architectural decisions in the audio path.

### 6.4 Library index

MPD's own database is adequate but awkward to query for a browsing UI. Maintain
a parallel SQLite index (`src/library/`) built with taglib during scan, with
tables for artists, albums, tracks, playlists, and a `mtime` column so rescans
are incremental.

Trigger a rescan when the USB gadget disconnects (§7) — that's the only moment
the library can have changed.

---

## 7. USB-C: charging and host connectivity

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
├── Now Playing        (only shown when something is loaded)
├── Settings
│   ├── Audio Output   (DAC / Bluetooth)
│   ├── Bluetooth      (scan, pair, connect)
│   ├── Backlight      (brightness, timeout)
│   ├── Haptics        (off / light / strong)
│   ├── Sleep Timer
│   └── About          (version, battery, storage, IP)
└── Extras             (reserved — see §11)
```

### 8.2 Input mapping

| Input | Action |
|---|---|
| Wheel rotate | Move selection / scrub in Now Playing / volume when held |
| Center | Select / cycle Now Playing display mode |
| Menu | Back one level; from root, sleep |
| Play/Pause | Toggle; long press = sleep |
| Next | Next track; hold = seek forward |
| Prev | Restart track, or previous if within 2 s; hold = seek back |

**Scroll acceleration is what makes this feel right or wrong.** Track angular
velocity across the last ~150 ms and apply a non-linear multiplier so a fast
flick jumps many rows. Without it, a 2000-song list is unusable. Budget real
tuning time for this; it is the single highest-leverage feel detail in the
project. Make the curve parameters live-tunable over the ACM serial console so
you can adjust without a rebuild.

Add the alphabet-scrub overlay for long lists: past a velocity threshold, show
a large centred letter and jump by first-letter index.

### 8.3 Rendering discipline

- No full-screen redraws outside screen transitions.
- Album art decoded once, cached at display size, never re-decoded on scroll.
- Now Playing progress updates at 1 Hz, not per-frame.
- Backlight fades to off on an idle timer; input at any brightness level wakes
  it without also registering as a selection.

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

### Phase 3 — Audio
Wire the PCM5102A. `speaker-test` through the DAC. Install and configure MPD.
Play a FLAC via `mpc`. Confirm bit-perfect: play a 24/96 file, check
`/proc/asound/card0/pcm0p/sub0/hw_params` reports 96000 and S24, not resampled.
**Accept:** 24/96 FLAC plays with no resampling and no dropouts under simulated
UI load.

### Phase 4 — UI integration
Wire the wheel into LVGL as an encoder indev. Build the screen graph. Connect
to MPD. Implement scroll acceleration and tune it.
**Accept:** navigate a 1000+ track library and start playback using only the
click wheel, with no SSH session open.

### Phase 5 — Power and USB
Build the charging front end. Fuel gauge over I²C, battery indicator in the UI.
Composite gadget + the §7.3 state machine. Sleep/wake path.
**Accept:** plug into a PC — mounts as a drive, library dismounts cleanly;
unplug — library remounts and rescans; copy a new album across and it appears
in the UI without a reboot. Battery percentage tracks reality within 5%.

### Phase 6 — Bluetooth (optional)
BlueZ + PipeWire, pairing UI, output switching. LibrePods only as a separate
timeboxed spike per §6.3.
**Accept:** pair AirPods from the device UI and switch output mid-track without
restarting MPD.

### Phase 7 — Hardening
Read-only rootfs with an overlayfs for config and the SQLite index. Buildroot
image if boot time still isn't acceptable. Watchdog. Crash recovery that
restores playback position.
**Accept:** pull the battery mid-playback 20 times; the device boots cleanly
every time and resumes within 2 seconds of where it stopped.

---

## 10. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Click wheel is not 4th gen | Medium | Sniffer in Phase 2 detects it immediately; §4.4 |
| pigpio drops edges under audio load | Medium | 1 µs sample rate; RP2040 fallback per §4.6 |
| 512 MB RAM insufficient | Low | zram; cap LVGL buffers; MPD DB on disk not memory |
| SPI display too slow for scroll | Low | Partial redraw is mandatory, not optional |
| Boot time unacceptable | High | Never fully power down — sleep instead. Buildroot as plan B |
| LibrePods port is a swamp | High | Timeboxed, isolated, entirely optional |
| exFAT corruption from bad unmount | Medium | §7.3 state machine; NCM-only fallback in §7.4 |
| Boost converter browns out on Wi-Fi TX | Medium | Size for 1.5 A peak; bulk cap at the Pi's 5 V pin |

---

## 11. Explicitly out of scope for v1

Keep these out of the critical path. Note them, build the structure to allow
them, implement none of them until Phase 7 passes.

- Streaming service clients
- Podcast support with position tracking
- Games (the original had Brick — obvious later addition, obviously not now)
- Voice memos / recording
- Wi-Fi sync from a home server
- Equaliser
- Any web UI

---

## 12. Licensing

The click wheel decoding approach derives from
`dupontgu/retro-ipod-spotify-client`, **Apache-2.0**. If any of that code is
carried over rather than reimplemented from the documented protocol, retain the
license header and attribution, and note it in `README.md`. LVGL is MIT.
Reimplementing from the protocol description in §4 avoids the question
entirely, and is recommended for other reasons anyway.

Note that the `-Pod` suffix is Apple trademark territory. Fine for a personal
project on GitHub; reconsider the name before selling boards or kits.

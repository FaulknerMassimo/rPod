<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/wordmark-on-dark.svg">
    <img src="docs/logo/wordmark-on-light.svg" alt="rPod" width="260">
  </picture>
</h1>

An iPod-style music player on a Raspberry Pi Zero 2 W, with an original
4th-gen iPod click wheel, a 2" 320×240 ST7789V LCD and a PiSugar 2
battery. MPD plays the library to Bluetooth headphones. A web UI uploads
music and videos and reaches the settings the wheel can't.

Full spec, hardware BOM, GPIO map, and phased build plan: [`docs/PLAN.md`](docs/PLAN.md).

## Setting up

This takes a bare Raspberry Pi Zero 2 W, the parts in §1, and a Linux PC,
and ends with an rPod that boots to the menu, plays your music to Bluetooth
headphones or AirPlay speakers, and has a web page for uploads and settings.

rPod is **cross-compiled on your PC** and pushed to the Pi over SSH. The
Pi's 512 MB of RAM and its CPU make building there painful (minutes for the
UI alone, and it can run out of memory), so the guide never does.

Reference docs when something here isn't enough: [`PLAN.md`](docs/PLAN.md) (the
spec, with the reasoning behind every setting), [`wiring.md`](docs/wiring.md),
[`clickwheel-protocol.md`](docs/clickwheel-protocol.md).

### 1. What you need

| Part | Notes |
|---|---|
| Raspberry Pi Zero 2 W | The *2* W: the original Zero W is too slow. Header pins soldered on. |
| microSD card | 32 GB or more; it holds the music. A good A1/A2 brand. |
| 4th-generation iPod click wheel | The original Apple part, with its 8-pin flex cable. Other generations use a different protocol (PLAN §4.4). |
| 8-pin FPC breakout | For the wheel's flex cable. |
| Waveshare 2" LCD module | ST7789V, 240×320, SPI, 8-pin cable. |
| PiSugar 2 | Battery, charger and RTC under the Pi. Optional: without it the battery shows `--%` and everything else works, on USB power. |
| Jumper wires, soldering iron | |
| Bluetooth headphones or an AirPlay speaker | The Zero 2 W has no headphone jack, and there's no DAC (yet). |

The PC needs Linux (anything with a recent clang), plus:

```sh
# Debian/Ubuntu names; use your distribution's
sudo apt install git make clang lld rsync curl python3 openssh-client
```

`make sim` (the desktop simulator) additionally wants SDL2, libmpdclient,
libcurl, libsystemd and FFmpeg's development packages. It isn't needed to
set up the device, but it's how you try UI changes without hardware.

Get the code, with LVGL:

```sh
git clone --recurse-submodules <this repository> rPod
cd rPod
```

(If you cloned without `--recurse-submodules`, run `git submodule update --init`.)

### 2. Wire it up

[![Pi Zero 2 W wired to the 2" display and the click wheel](docs/wiring-zero2w.svg)](docs/wiring-zero2w.svg)

Follow [`wiring.md`](docs/wiring.md) and its picture
([`wiring-zero2w.svg`](docs/wiring-zero2w.svg)). Three things trip people up:

- **Display D/C goes to GPIO 24 (pin 18)**, not GPIO 25 as Waveshare's own
  guide has it: GPIO 25 is the click wheel's DATA line.
- **Backlight goes to GPIO 13 (pin 33)**, not GPIO 18.
- **Everything is 3.3 V.** The display's VCC and the click wheel's power
  must come from a 3V3 pin, never 5 V.

Wire it all before the first boot and check each connection twice; there are
no resistors to fit (the wheel's pull-ups are the Pi's own).

Fit the PiSugar 2 under the Pi (its pogo pins touch the Pi's underside test
pads) last. It's on GPIO 2/3, which nothing else uses.

### 3. Flash Raspberry Pi OS

1. In Raspberry Pi Imager choose **Raspberry Pi OS Lite (64-bit)**, trixie
   (Debian 13) or newer. No desktop.
2. In the OS customisation step set:
   - hostname **`rpod`** (the guide and the Makefile assume `rpod.local`),
   - username **`rpod`** and a password,
   - your Wi-Fi network (the Zero 2 W is 2.4 GHz only),
   - **SSH enabled**, ideally with your public key.
3. Boot the Pi and log in from your PC: `ssh rpod@rpod.local`.
   mDNS can be unreliable on some networks. If `rpod.local` won't resolve,
   find the Pi's IP address in your router and use it in step 6's overrides.
4. rPod's `make` targets run `sudo` on the Pi without a password, and the
   Imager's image asks for one. Allow it:

   ```sh
   echo 'rpod ALL=(ALL) NOPASSWD: ALL' | sudo tee /etc/sudoers.d/010_rpod-nopasswd
   sudo chmod 440 /etc/sudoers.d/010_rpod-nopasswd
   ```

5. Copy your SSH key over if you didn't give Imager one:
   `ssh-copy-id rpod@rpod.local`. Every command below makes several SSH
   connections, so a key saves a lot of typing.

### 4. Install the base system on the Pi

From the repository, on your PC:

```sh
make pi-setup
```

This runs `system/setup.sh` on the Pi, which takes **10 to 20 minutes** (it
compiles pigpio, and installs a few hundred packages). It's safe to re-run.
It runs as a systemd unit, so if your SSH session or the Pi's Wi-Fi drops,
nothing is lost: pick the output back up with
`ssh rpod@rpod.local journalctl -fu rpod-setup`.

It sets up:

- the packages rPod needs, including the `-dev` ones your PC's cross-build
  will link against, and **pigpio** (built from source into `/usr/local`),
- the boot config: SPI, I²C, the ST7789V panel's overlay
  (`system/config.txt.d/rpod.txt` and `system/overlays/rpod-panel.dts`), the
  RTC, and kernel options in `cmdline.txt`. Originals are kept as
  `config.txt.orig` and `cmdline.txt.orig`,
- **fast boot** (`system/boot/`): the rPod logo is up about 10 seconds after
  power-on and the menu at about 22,
- **MPD**, on a Unix socket only, with the library in `/media/music`
  (and `/media/videos`, beside it),
- **Bluetooth audio**: BlueZ plus a system-wide PipeWire for MPD to play
  into,
- **AirPlay**: OwnTone, from its own apt archive, fed by MPD,
- the **PiSugar 2**'s power policy and RTC.

When it says the boot config changed, reboot:

```sh
ssh rpod@rpod.local sudo reboot
```

The panel will stay dark after this: nothing draws on it until rPod is
installed.

### 5. Build and deploy rPod

All of this is on your PC, in the repository. Each step needs the Pi
reachable over SSH.

```sh
# The Classic themes' fonts and bitmaps (optional; see the note below)
make ipod-assets

# Copy the Pi's headers and libraries here, for cross-compiling.
# Once, and again after installing new -dev packages on the Pi.
make sysroot

# The UI, the boot logo, and the Pi's service files
make deploy-run

# The click wheel daemon
make deploy-wheel

# The web UI, and the video converter
make deploy-web
```

After `make deploy-run` the boot logo and then the main menu should be on
the panel. After `make deploy-wheel` the click wheel drives it.

**If `rpod.local` won't resolve**, override the address on every command
(keeping the host key from the first connection):

```sh
make deploy-run PI_HOST=192.168.1.100 SSH="ssh -o HostKeyAlias=rpod.local"
```

**About `make ipod-assets`.** Glass, rPod's own theme, needs nothing. The
*Classic* and *Classic Dark* themes imitate the 4th-gen iPod and use its
fonts and bitmaps, which are Apple's, so they aren't in the repository.
This target downloads the iPod's firmware from Apple, checks its hash, and
extracts them locally. They are for your own use: don't commit them or
redistribute a build that contains them. Skip the step and Classic still
works, with Montserrat standing in. Run it before `make deploy-run` so the
build includes them.

`make build` alone just compiles, without a Pi. Run `make test` for the
host-side unit tests.

### 6. Put music on it

The library is `/media/music`. Either:

- **the web UI** (easiest): open `http://rpod.local/` in a browser and
  drag song files or whole album folders onto the page. It sorts them,
  makes the names safe, and tells MPD to update. Videos go in the same way,
  and rPod converts them to fit the screen. Or
- **copy files** to the Pi directly, then update MPD's database:

  ```sh
  rsync -av ~/Music/ rpod@rpod.local:/media/music/
  ssh rpod@rpod.local mpc update
  ```

FLAC, MP3, AAC/M4A, Ogg and anything else MPD's decoders handle all work.
Embedded cover art is read straight from FLAC files, and the first
browse after adding a big library is slower while rPod builds its cover
cache in the background.

To require a password on the web UI, put `RPOD_WEB_PASSWORD=yourpassword`
in `/etc/rpod/env` on the Pi, then `sudo systemctl restart rpod-web`.

### 7. Audio

**Bluetooth headphones.** Put them in pairing mode, then on the device:
Settings → Bluetooth → turn it on, pick them from the list, and Pair. They
reconnect by themselves afterwards. Bluetooth audio is lossy, so MPD
re-encodes what it plays for them. AirPods get extras (battery, noise
control, ear detection).

**AirPlay.** In Now Playing, "Play On" lists the AirPlay speakers on your
network (HomePods, Apple TVs, AirPlay receivers). Pick one. Some, like an
Apple TV, ask for a one-time PIN the first time; there's no screen
for it on the device, so enter it from a shell on the Pi while the TV
shows it:

```sh
curl -X PUT -d '{"pin":"1234"}' http://127.0.0.1:3689/api/verification
```

The Pi's Wi-Fi and the speaker must be on the same network, and the
network must let mDNS through.

### 8. Day to day

| Task | Command |
|---|---|
| Update rPod after pulling new code | `make deploy-run` (add `deploy-web` / `deploy-wheel` if they changed) |
| Watch the UI's log | `ssh rpod@rpod.local journalctl -fu rpod` |
| Check the wheel | `ssh rpod@rpod.local wheel-test-client` |
| Restart the UI | `ssh rpod@rpod.local sudo systemctl restart rpod` |
| Power off | Settings → Shut Down (on a PiSugar, it cuts the power after) |

The device turns itself off after a spell idle on battery (Settings →
Auto-Off), and when the battery runs out. Plugging it into USB charges it.

### 9. Troubleshooting

**The panel is black or white and stays so.** Power off and re-check the
wiring against [`wiring.md`](docs/wiring.md), especially D/C on pin 18 and the
3.3 V supply. Then `ssh rpod@rpod.local` and look at
`sudo vclog -m | grep -i 'unknown dtparam'` (a mistyped boot-config line)
and `ls /dev/rpod-panel` (the panel's driver bound). If the panel is
black but `/sys/bus/spi/devices/spi0.0/statistics/bytes_tx` keeps rising,
re-bind its driver: stop rpod, write `spi0.0` to
`/sys/bus/spi/drivers/fb_st7789v/unbind` then `bind`, start rpod. Or just
reboot.

**The picture tears when it scrolls.** Try `rotate=180` instead of
`rotate=0` on the `dtoverlay=fbtft` line of `/boot/firmware/config.txt`
(PLAN §5.3).

**The wheel does nothing.** `journalctl -u rpod-wheel`; run
`wheel-test-client` and turn the wheel. Nothing at all usually means
wiring on GPIO 23 (CLOCK) or 25 (DATA) or a wheel from the wrong
generation. If it worked and stopped, `sudo systemctl restart rpod-wheel`.

**The menu never appears and the logo stays.** rPod reports ready to
systemd after its first frame, and systemd kills it after 30 seconds if it
doesn't. `journalctl -u rpod -b` says why. MPD takes a while to start on
the Zero 2 W (7 to 9 seconds), which is normal.

**`rpod.local` doesn't resolve, or SSH drops.** Use the IP address
(section 5). The Zero 2 W's Wi-Fi is 2.4 GHz and its antenna is small:
keep it near the router for setup.

**The clock says 2002 or the wrong time until Wi-Fi connects.** That's an
unset PiSugar RTC. Once the Pi gets online, it syncs and writes the RTC
back.

**Boot config changes.** Try them through `tryboot` first
(`/boot/firmware/tryboot.txt`, then `sudo reboot "0 tryboot"`): that one
boot reads it, and a power cycle goes back to `config.txt`, so a bad
change can't strand the Pi.

**Editing the boot config.** The firmware silently truncates `config.txt`
lines at 98 characters. If you edit the display lines, keep each `dtparam=`
short, and check `sudo vclog -m` for `Unknown dtparam` afterwards.

### 10. Hacking on it

- `make sim` runs the same UI on your PC in an SDL window, against a local
  MPD (`make mpd-dev-conf && make mpd-dev` first). The keyboard stands in
  for the wheel: arrows rotate, Enter selects, M is Menu, Space is
  Play/Pause, N and P are Next and Previous.
- `make web-sim` runs the web UI against the simulator.
- `CLAUDE.md` and [`PLAN.md`](docs/PLAN.md) hold the hard-won hardware and
  simulator notes. Read the relevant section before changing how the
  display, the wheel, the boot, or MPD is driven.

## Layout

- `src/` — on-device application (UI, input, MPD client, Bluetooth,
  battery, video).
- `daemon/` — `rpod-wheel`, the privileged click wheel decoder, and the
  `wheel_protocol.h`/`wheel_bits.h` headers shared with its clients; and
  `rpod-splash`, the logo on the screen while the Pi boots.
- `web/` — `rpod-web`, the web UI's daemon, and the page it serves
  (`web/static/`).
- `system/` — `setup.sh` (`make pi-setup`), boot config fragments, systemd
  units, udev rules, and MPD, Bluetooth, PiSugar and boot-time setup.
- `tools/` — `wheel-sniff` (protocol analysis), `wheel-test-client` (prints
  normalised wheel events), `fb-test` (raw framebuffer colour/orientation
  check), `fake-airpods.py` (AirPods stand-in for the sim), and `sim/`
  (desktop UI harness).
- `tests/` — host-run unit tests (`make test`).
- `docs/logo/` — the rPod wordmark, light and dark, and its square mark.
- `third_party/` — vendored LVGL.

## License

Copyright © 2026 Massimo Faulkner. rPod's code, docs and configuration are
licensed under [Creative Commons Attribution-NonCommercial 4.0
International](https://creativecommons.org/licenses/by-nc/4.0/) (CC BY-NC
4.0) — full text in [`LICENSE`](LICENSE).

You're free to use, copy, modify and share it, as long as:

- **you credit it** — name the author, link to this project and the
  license, and say if you changed anything. For example: "Based on rPod by
  Massimo Faulkner, licensed under CC BY-NC 4.0."
- **you don't use it commercially** — no selling it, or kits, boards or
  products built from it, without asking first.

Third-party parts keep their own licenses: LVGL (`third_party/lvgl`) is MIT,
and the Montserrat fonts in `src/ui/fonts/` and `web/static/fonts/` are SIL
OFL 1.1 (`web/static/fonts/Montserrat-OFL.txt`). The AirPods
pictures in `src/ui/airpods_art_data.c` come from Wikimedia Commons — the
buds from a photo by Burkhard Mücke (CC BY-SA 4.0, which that image keeps),
the case from one by Jackienyyy (CC0) — and the AirPods glyph is
MingCute's (Apache-2.0); see `tools/airpods-art.py`. Click wheel
decoding derives from `dupontgu/retro-ipod-spotify-client` (Apache-2.0) —
see `docs/PLAN.md` §12. AirPods support is reimplemented from the protocol
notes in LibrePods (`kavishdevar/librepods`, GPL-3.0); none of its code is
included.

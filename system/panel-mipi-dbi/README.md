# panel-mipi-dbi firmware for the Waveshare 1.44" LCD HAT

The Waveshare 1.44" LCD HAT (ST7735S, 128×128) is driven on-device through the
mainline **`panel-mipi-dbi`** DRM driver — the `docs/PLAN.md` §5.1 preferred
path — rather than the deprecated `fbtft` staging driver. fbtft's `fb_st7735r`
writes to a fixed GRAM window at (0,0) and so cannot compensate for the offset
of the 128×128 glass inside the controller's 132×162 GRAM, leaving a few pixels
of garbage on two edges. `panel-mipi-dbi` takes explicit `x-offset`/`y-offset`
device-tree params and handles the SPI byte order correctly (no LVGL swap).

`panel-mipi-dbi` loads its init sequence from a firmware blob at
`/lib/firmware/panel.bin`.

## Build the blob

`mipi-dbi-cmd` (vendored here, CC0, from
<https://github.com/notro/panel-mipi-dbi>) compiles the plain-text init sequence
into the binary format the driver expects:

```sh
./mipi-dbi-cmd panel.bin st7735s-waveshare144.txt
# inspect / round-trip:
./mipi-dbi-cmd panel.bin
```

## Install on the Pi

```sh
sudo install -m 644 panel.bin /lib/firmware/panel.bin
```

Then enable the overlay (see `system/config.txt.d/rpod.txt`, Board B):

```ini
dtoverlay=mipi-dbi-spi,spi0-0,speed=32000000
dtparam=width=128,height=128
dtparam=x-offset=1,y-offset=2
dtparam=reset-gpio=27,dc-gpio=25,backlight-gpio=24
```

and disable `vc4-kms-v3d` so the SPI panel is deterministically `/dev/fb1`
(`RPOD_FB=/dev/fb1` in `/etc/rpod/env`). Reboot.

## Values pinned against the real panel (tools/fb-test)

| What            | Value       | How found                                         |
|-----------------|-------------|---------------------------------------------------|
| Column offset   | `x-offset=1`| ruler mode: red ring flush to left/right at 1     |
| Row offset      | `y-offset=2`| ruler mode: red ring flush to top/bottom at 2     |
| Orientation     | MADCTL `0x68` (MV\|MX\|BGR) | grid/UI: upright, not mirrored  |
| Colour order    | BGR (0x08 bit) | fill mode: R→G→B read back correctly           |
| Inversion       | INVOFF (0x20)  | fill mode: colours not negative                |

`tools/fb-test.c` grew `grid` and `ruler` modes for exactly this bring-up:
`fb-test /dev/fb1 ruler` draws concentric colour rings so a missing/garbage
edge and its pixel count are directly readable.

Backlight note: on this DRM path the panel backlight and pixel output only turn
on once something *unblanks* the fbdev (a modeset). rPod does this itself via
LVGL's `FBIOBLANK` on startup; a bare `fb-test` run needs a manual
`echo 0 | sudo tee /sys/class/graphics/fb1/blank` (and the backlight comes up
with it).

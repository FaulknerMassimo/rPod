# Wiring

Logical GPIO assignments are the source of truth in `docs/PLAN.md` §1.2 —
this doc adds the physical 40-pin header position for each one, since BCM
GPIO numbers alone aren't enough to place a wire on the header.

## Pi Zero 2 W: display and click wheel

![Pi Zero 2 W wired to the 2" display and the click wheel](wiring-zero2w.svg)

Top view, component side up: pin 1 is the inner-row pin at the microSD
end, and the outer row along the board's edge holds the even pins. The Pi
3B dev board has the same header, so the pin numbers hold there too.
`tools/wiring-diagram.py` draws the picture: rerun it after changing a pin.

### Display: Waveshare 2" LCD module

Its 8-pin connector, in the cable's order:

| Module pin | Pi pin | BCM GPIO | Notes |
|---|---|---|---|
| VCC | 17 | 3V3 | VCC must match the logic level: 3.3 V, not 5 V |
| GND | 20 | — | Any GND pin |
| DIN | 19 | 10 | SPI0 MOSI |
| CLK | 23 | 11 | SPI0 SCLK |
| CS | 24 | 8 | SPI0 CE0 |
| DC | 18 | 24 | Not Waveshare's pin 22 (GPIO 25 is the wheel's DATA, below) |
| RST | 13 | 27 | |
| BL | 33 | 13 | PWM1. Not Waveshare's pin 12 (GPIO 18 is I²S) |

`system/config.txt.d/rpod.txt` expects exactly these
(`reset_pin=27,dc_pin=24,led_pin=13`).

### Click wheel: 8-pin ribbon

The 4th-gen wheel's flex cable goes into an 8-pin FPC breakout. Its pin
numbers are the ones printed on the ribbon, from Jason Garr's notes on the
same wheel
([iPod clickwheel hack](https://jasongarr.wordpress.com/project-pages/ipod-clickwheel-hack/)):

| Ribbon pin | Signal | Pi pin | BCM GPIO |
|---|---|---|---|
| 1 | Power (the iPod's battery rail) | 1 | 3V3 |
| 2 | CLOCK | 16 | 23 |
| 3 | Enable: tie to ribbon pin 1 | — | — |
| 4, 5 | Not connected | — | — |
| 6 | DATA | 22 | 25 |
| 7 | Enable: tie to ribbon pin 1 | — | — |
| 8 | GND | 6 | — |

The wheel is a 3.3 V part: never give it 5 V. CLOCK and DATA need pull-ups,
and `rpod-wheel` turns on the Pi's internal ones, so no resistors.

## Watch for this: display D/C vs click wheel DATA

**Waveshare's own wiring guide for this 2" ST7789V panel defaults D/C to
GPIO 25** — which collides with the click wheel's DATA line. GPIO 23/25/26
are deliberately pinned to match the sPot reference click wheel schematic
(`docs/PLAN.md` §1.2), so the panel's D/C line was moved to **GPIO 24**
instead, and `system/config.txt.d/rpod.txt` (`dc_pin=24`) already assumes
that override. Wire the panel's D/C pin to physical **pin 18** (GPIO 24),
not physical pin 22 (GPIO 25) — GPIO 25 is reserved for the wheel.

## Full pin map

| Signal | Peripheral | BCM GPIO | Physical pin |
|---|---|---|---|
| I²C SDA | Fuel gauge | 2 | 3 |
| I²C SCL | Fuel gauge | 3 | 5 |
| SPI0 CE0 | LCD chip select | 8 | 24 |
| SPI0 MOSI | LCD data | 10 | 19 |
| SPI0 SCLK | LCD clock | 11 | 23 |
| LCD D/C | GPIO out | 24 | 18 |
| LCD RESET | GPIO out | 27 | 13 |
| LCD backlight | Hardware PWM1 | 13 | 33 |
| I²S BCK | PCM_CLK | 18 | 12 |
| I²S LRCK | PCM_FS | 19 | 35 |
| I²S DIN | PCM_DOUT | 21 | 40 |
| Click wheel CLOCK | GPIO in, pull-up | 23 | 16 |
| Click wheel DATA | GPIO in, pull-up | 25 | 22 |
| Haptic motor | GPIO out | 26 | 37 |
| USB VBUS sense | GPIO in, via 100k/100k divider | 22 | 15 |
| Hold switch | GPIO in, pull-up | 16 | 36 |
| Power button | GPIO in, pull-up | 17 | 11 |

Notes (`docs/PLAN.md` §1.2):
- LCD MISO is unconnected; the ST7789V read path is not used.
- Backlight uses PWM1 on GPIO 13. Do not use GPIO 12 or 18 — 18 is I²S.
- Both click wheel lines need pull-ups; the Pi's internal pull-ups are
  sufficient, no external resistors required.

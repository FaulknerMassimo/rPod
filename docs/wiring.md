# Wiring

Logical GPIO assignments are the source of truth in `docs/PLAN.md` §1.2 —
this doc adds the physical 40-pin header position for each one, since BCM
GPIO numbers alone aren't enough to place a wire on the header.

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

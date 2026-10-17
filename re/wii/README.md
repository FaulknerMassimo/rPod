# Wii theme art

The Wii theme (`docs/PLAN.md` §8.4, `src/ui/wii.c`) draws with the pictures of
the Rockbox **"Wii_menu"** theme by Monica G. (CC BY-SA 3.0): the channel
tiles, the bar's wave, the playback panels, Now Playing's backdrop and Mii, and
the digital-7 clock numerals.

That licence is share-alike, which rPod's (CC BY-NC 4.0) can't carry, and the
pictures are the Wii's besides -- so, like the iPod's, they are not in the
repository. Only the converter is:

```
python3 -I re/wii/tools/extract.py Wii_menu.zip re/wii/out [--png]
```

(`make wii-assets` does this, from `Wii_menu.zip` in the repository root.) It
needs nothing beyond python3, and writes `re/wii/out/lvgl/wii_images.{h,c}`:
LVGL 9 images (RGB565, or RGB565A8 where the theme's magenta key is
transparency) and the clock's digits as A8 masks. `--png` also writes
previews of the three backdrop crops to `re/wii/out/png/`. The Makefile
compiles `wii_images.c` in when it's there (`WII_ASSETS`), and defines
`RPOD_WII_ASSETS`.

`tools/wiire.py` reads the theme's files: 1/4/8/24-bit BMPs, Rockbox's `RB12`
bitmap fonts (4 bits a pixel, row by row, low nibble first, 0 = ink) and writes
PNGs. `tools/extract.py` has the crops: the sbs backdrop's bottom wave
(rows 170-240) and its right-hand panels (x 233-303, with the pointing hand
the backdrop has baked in rebuilt from the panels' mirror image), the whole
wps backdrop, and the sprite sheets cut into their frames.

Which sheet frame means what comes from the theme's own `.wps`/`.sbs`
(`%xd(playmodes,%mp)` and so on): playmodes are stop, play, pause, ...;
repeat is off, all, one, ...; the 25 battery frames are 12 draining, 12
charging, "charged" (`src/ui/wii_frames.h`, tested by `tests/test_wii.c`).

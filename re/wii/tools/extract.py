#!/usr/bin/env python3
"""Turns the Rockbox "Wii_menu" theme (by Monica G., CC BY-SA 3.0) into the
LVGL image sources the Wii theme draws with.

    python3 -I re/wii/tools/extract.py Wii_menu.zip re/wii/out [--png]

writes re/wii/out/lvgl/wii_images.{h,c} and, with --png, previews under
re/wii/out/png/. Standard library only. The art is licensed CC BY-SA 3.0 and
rPod is CC BY-NC 4.0, so it is never committed (docs/PLAN.md §8.4, §12): this
runs on the user's own copy of the theme, like `make ipod-assets` does for
the iPod's.

Where each piece comes from (coordinates in the theme's 320x240 backdrops):
  wii_bar     the sbs backdrop's bottom wave, y 170-240 (the clock sits in its dip)
  wii_column  the sbs backdrop's three right-hand panels (battery, playback,
              repeat and shuffle)
  wii_np_bg   the whole wps backdrop: Now Playing
  wii_tile*   wii_iconset(_select).bmp, 9 tiles of 71x55, normal and selected
  wii_battery 25 frames of 42x16, volume 21 of 36x36, playmode 9 of 35x35,
  wii_repeat  5 of 33x33, wii_shuffle 2, wii_lock 3 of 12x16, wii_mii 2
  wii_digit   the clock's 0-9 and ':' from 34-digital7mono.fnt, as A8 masks
"""

import argparse
import os
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wiire  # noqa: E402

BASE = ".rockbox/wps/Wii_menu/"
BAR = (0, 170, 320, 70)
COLUMN = (233, 5, 70, 166)


def frames(img, fw, fh):
    return [img.crop(0, i * fh, fw, fh) for i in range(img.h // fh)]


def without_hand(img, x0, y0):
    """The backdrop has the Wii's pointing hand baked in beside the middle
    panel. The panels are symmetric (x 236-298), so that stretch of rows is
    rebuilt from the clean left half, mirrored (the crop starts at x0, y0)."""
    px = list(img.px)
    for y in range(94, 127):
        for x in range(286, x0 + img.w):
            m = 534 - x - x0
            px[(y - y0) * img.w + (x - x0)] = img.px[(y - y0) * img.w + m] if 0 <= m < img.w else (255, 255, 255, 255)
    return wiire.Image(img.w, img.h, px)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


class Emitter:
    def __init__(self):
        self.c = []
        self.h = []
        self.arrays = {}

    def _bytes(self, name, data):
        self.c.append(f"static const uint8_t {name}_map[] = {{")
        for i in range(0, len(data), 16):
            self.c.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        self.c.append("};")

    def image(self, sym, img, comment):
        """RGB565 when fully opaque, else RGB565A8 (LVGL's colour plane, then alpha)."""
        has_alpha = any(p[3] != 255 for p in img.px)
        color = bytearray()
        for r, g, b, a in img.px:
            v = rgb565(r, g, b) if a else 0
            color += bytes((v & 255, v >> 8))
        data = bytes(color) + (bytes(p[3] for p in img.px) if has_alpha else b"")
        cf = "LV_COLOR_FORMAT_RGB565A8" if has_alpha else "LV_COLOR_FORMAT_RGB565"
        self.c.append(f"/* {comment} {img.w}x{img.h} */")
        self._bytes(sym, data)
        self.c.append(f"const lv_image_dsc_t {sym} = {{")
        self.c.append(f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, .flags = 0, "
                      f".w = {img.w}, .h = {img.h}, .stride = {img.w * 2}}},")
        self.c.append(f"    .data_size = sizeof({sym}_map),")
        self.c.append(f"    .data = {sym}_map,")
        self.c.append("};\n")
        self.h.append(f"extern const lv_image_dsc_t {sym};")

    def mask(self, sym, w, h, alpha, comment):
        self.c.append(f"/* {comment} {w}x{h} */")
        self._bytes(sym, alpha)
        self.c.append(f"const lv_image_dsc_t {sym} = {{")
        self.c.append(f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0, "
                      f".w = {w}, .h = {h}, .stride = {w}}},")
        self.c.append(f"    .data_size = sizeof({sym}_map),")
        self.c.append(f"    .data = {sym}_map,")
        self.c.append("};\n")
        self.h.append(f"extern const lv_image_dsc_t {sym};")

    def series(self, name, imgs, comment):
        for i, im in enumerate(imgs):
            self.image(f"wii_img_{name}_{i}", im, f"{comment} {i}")
        self.arrays[name] = len(imgs)


def build(zf_path, outdir, want_png):
    with tempfile.TemporaryDirectory() as tmp, zipfile.ZipFile(zf_path) as z:
        names = {n: n for n in z.namelist()}
        for need in (".rockbox/fonts/34-digital7mono.fnt", BASE + "Wii_menu_sbs.bmp"):
            if need not in names:
                sys.exit(f"{zf_path}: no {need} -- is this the Wii_menu theme?")
        z.extractall(tmp)
        bmp = lambda n: wiire.read_bmp(os.path.join(tmp, BASE + n))  # noqa: E731
        sbs, wps = bmp("Wii_menu_sbs.bmp"), bmp("Wii_menu_wps.bmp")
        glyphs = wiire.read_rb12_glyphs(os.path.join(tmp, ".rockbox/fonts/34-digital7mono.fnt"), "0123456789:")

        e = Emitter()
        e.image("wii_img_bar", sbs.crop(*BAR).opaque(), "sbs bottom wave")
        column = without_hand(sbs.crop(*COLUMN).opaque(), COLUMN[0], COLUMN[1])
        e.image("wii_img_column", column, "sbs right-hand panels")
        e.image("wii_img_np_bg", wps.opaque(), "wps backdrop")
        e.series("tile", [t.opaque() for t in frames(bmp("wii_iconset.bmp"), 71, 55)], "tile")
        e.series("tile_sel", [t.opaque() for t in frames(bmp("wii_iconset_select.bmp"), 71, 55)], "selected tile")
        e.series("battery", frames(bmp("battery.bmp"), 42, 16), "battery")
        e.series("volume", frames(bmp("volume.bmp"), 36, 36), "volume")
        e.series("playmode", frames(bmp("playmodes.bmp"), 35, 35), "playmode")
        e.series("repeat", frames(bmp("repeat_status.bmp"), 33, 33), "repeat")
        e.series("shuffle", frames(bmp("shuffle.bmp"), 33, 33), "shuffle")
        e.series("lock", frames(bmp("lock.bmp"), 12, 16), "lock")
        e.series("mii", [bmp("mii_album.bmp"), bmp("mii_alternate.bmp")], "mii")
        digit_syms = []
        for ch in "0123456789:":
            w, h, _asc, alpha = glyphs[ch]
            sym = f"wii_img_digit_{'colon' if ch == ':' else ch}"
            e.mask(sym, w, h, alpha, f"clock '{ch}'")
            digit_syms.append(sym)

        if want_png:
            pdir = os.path.join(outdir, "png")
            os.makedirs(pdir, exist_ok=True)
            wiire.write_png(os.path.join(pdir, "bar.png"), sbs.crop(*BAR))
            wiire.write_png(os.path.join(pdir, "column.png"), column)
            wiire.write_png(os.path.join(pdir, "np_bg.png"), wps)

    lvgl = os.path.join(outdir, "lvgl")
    os.makedirs(lvgl, exist_ok=True)
    head = ["/* The Wii theme's art, from the Rockbox \"Wii_menu\" theme by Monica G.",
            " * (CC BY-SA 3.0), converted by re/wii/tools/extract.py. Never committed. */"]
    with open(os.path.join(lvgl, "wii_images.h"), "w") as f:
        f.write("#pragma once\n#include \"lvgl.h\"\n\n" + "\n".join(head) + "\n\n")
        f.write("\n".join(e.h) + "\n\n")
        for name, n in e.arrays.items():
            f.write(f"#define WII_{name.upper()}_COUNT {n}\n")
            f.write(f"extern const lv_image_dsc_t *const wii_img_{name}[WII_{name.upper()}_COUNT];\n")
        f.write("\n/* '0'-'9', then ':' */\nextern const lv_image_dsc_t *const wii_img_digits[11];\n")
    with open(os.path.join(lvgl, "wii_images.c"), "w") as f:
        f.write("\n".join(head) + "\n#include \"wii_images.h\"\n\n")
        f.write("\n".join(e.c) + "\n")
        for name, n in e.arrays.items():
            f.write(f"const lv_image_dsc_t *const wii_img_{name}[{n}] = {{\n")
            f.write("".join(f"    &wii_img_{name}_{i},\n" for i in range(n)) + "};\n")
        f.write("\nconst lv_image_dsc_t *const wii_img_digits[11] = {\n")
        f.write("".join(f"    &{s},\n" for s in digit_syms) + "};\n")
    print(f"wrote {lvgl}/wii_images.{{h,c}}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("zip", help="the Wii_menu.zip Rockbox theme")
    ap.add_argument("outdir", help="where lvgl/ (and png/) go")
    ap.add_argument("--png", action="store_true", help="also write previews")
    a = ap.parse_args()
    build(a.zip, a.outdir, a.png)

#!/usr/bin/env python3
"""Generates src/ui/airpods_art_data.c: the pictures ui/airpods_art.h hands
out in the Glass theme, from freely licensed sources.

  - The battery card's buds: "AirPods Modell Pro 1.jpg" by Burkhard Mücke,
    CC BY-SA 4.0, https://commons.wikimedia.org/wiki/File:AirPods_Modell_Pro_1.jpg
  - Its charging case: "AirPod pro.jpg" by Jackienyyy, CC0,
    https://commons.wikimedia.org/wiki/File:AirPod_pro.jpg
  - The status bar and HUD glyph: MingCute's "airpods-fill"
    (https://github.com/Richard9394/MingCute), Apache-2.0.

The photos are cut off their backgrounds (GrabCut, seeded by the wood's
colour for the buds and by a box for the case, whose ragged right edge is
replaced by its left one mirrored), made neutral white, and scaled down in
linear light. So the buds image is CC BY-SA 4.0 like its source: the
generated file says so. The classic themes' pixel art isn't from here; it's
drawn by hand in src/ui/airpods_art.c.

    python3 -m venv .venv && .venv/bin/pip install numpy scipy opencv-python-headless pillow
    .venv/bin/python -I tools/airpods-art.py

Needs rsvg-convert (librsvg) for the glyph. The photos are downloaded once
into .cache/airpods-art/ and checked against Commons' SHA-1s. --png DIR also
writes what was generated as PNGs, to look at.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import urllib.request

import cv2
import numpy as np
from PIL import Image, ImageOps
from scipy import ndimage as ndi

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BUDS = {
    "url": "https://upload.wikimedia.org/wikipedia/commons/3/39/AirPods_Modell_Pro_1.jpg",
    "sha1": "21ec47e065dcde200f40bb3e8c3be0184917cb6d",
}
CASE = {
    "url": "https://upload.wikimedia.org/wikipedia/commons/b/bb/AirPod_pro.jpg",
    "sha1": "612335d01deb20f21db3331fe8dd4d4f4e2eb82d",
}
# MingCute's airpods-fill, on its 24x24 grid; the drawing spans x 2-22, y 4-20.
GLYPH_PATH = ("M6.5 4A4.5 4.5 0 0 1 11 8.5V18a2 2 0 0 1-4 0v-5.027A4.5 4.5 0 1 1 6.5 4m11 0a4.5 "
              "4.5 0 1 1-.5 8.973V18a2 2 0 0 1-4 0V8.5A4.5 4.5 0 0 1 17.5 4")
GLYPH_BOX = (2, 4, 20, 16)

# Heights on the panel, as ui/airpods_notify.c lays the card out.
BUDS_H = 52
CASE_H = 40
GLYPH_H = 12

# The photos are worked on at this width, whatever their resolution.
WORK_W = 1920


def fetch(src, cache):
    path = os.path.join(cache, os.path.basename(src["url"]))
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        print(f"downloading {src['url']}", file=sys.stderr)
        req = urllib.request.Request(src["url"], headers={"User-Agent": "rpod-airpods-art/1.0"})
        with urllib.request.urlopen(req) as r, open(path + ".part", "wb") as f:
            f.write(r.read())
        os.rename(path + ".part", path)
    with open(path, "rb") as f:
        if hashlib.sha1(f.read()).hexdigest() != src["sha1"]:
            sys.exit(f"{path}: not the file this was made from (SHA-1 mismatch)")
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    im = im.resize((WORK_W, round(im.height * WORK_W / im.width)), Image.LANCZOS)
    return np.asarray(im)


def grabcut(rgb, mask=None, rect=None):
    bgr = np.ascontiguousarray(rgb[:, :, ::-1])
    bgd, fgd = np.zeros((1, 65)), np.zeros((1, 65))
    if rect is not None:
        mask = np.zeros(rgb.shape[:2], np.uint8)
        cv2.grabCut(bgr, mask, rect, bgd, fgd, 8, cv2.GC_INIT_WITH_RECT)
    else:
        cv2.grabCut(bgr, mask, None, bgd, fgd, 5, cv2.GC_INIT_WITH_MASK)
    return (mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD)


def largest(mask, n):
    lab, count = ndi.label(mask)
    sizes = ndi.sum(mask, lab, range(1, count + 1))
    return ndi.binary_fill_holes(np.isin(lab, 1 + np.argsort(sizes)[-n:]))


def smooth(mask, sigma=2.5):
    """Irons the cut's pixel-scale ragged edge into a smooth contour."""
    return ndi.gaussian_filter(mask.astype(np.float32), sigma) > 0.5


def cut_buds(rgb):
    """White plastic on orange wood: the wood is saturated, the buds aren't."""
    f = rgb.astype(np.float32) / 255
    mx, mn = f.max(2), f.min(2)
    sat = (mx - mn) / np.maximum(mx, 1e-6)
    seed = ndi.binary_opening((sat < 0.16) & (mx > 0.45), iterations=3)
    seed = ndi.binary_fill_holes(ndi.binary_closing(seed, iterations=6))
    m = np.full(mx.shape, cv2.GC_PR_BGD, np.uint8)
    m[ndi.binary_dilation(seed, iterations=6)] = cv2.GC_PR_FGD
    m[ndi.binary_erosion(seed, iterations=10)] = cv2.GC_FGD
    m[~ndi.binary_dilation(seed, iterations=25)] = cv2.GC_BGD
    return smooth(largest(grabcut(rgb, mask=m), 2))


def cut_case(rgb):
    """White on a light grey table, so GrabCut from a box around it -- which
    takes a bite out of its right side, where the table's glare is. The
    photo is square-on, so the left side mirrored stands in for it."""
    mask = largest(grabcut(rgb, rect=(390, 960, 1110, 840)), 1)
    xs = np.nonzero(mask.any(0))[0]
    x0, x1 = xs[0], xs[-1] + 1
    mid = (x0 + x1) // 2
    half = mask[:, x0:mid]
    mask[:, x1 - half.shape[1]:x1] = half[:, ::-1]
    return smooth(mask)


def to_linear(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def to_srgb(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)


def render(rgb, mask, h, white):
    """Crops to `mask`, greys it (white plastic, whatever the light's tint),
    puts its `white` percentile at white and scales it to `h` px tall, with
    a touch of sharpening to keep its edges at that size."""
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    lin = to_linear(rgb[y0:y1, x0:x1].astype(np.float32) / 255)
    a = mask[y0:y1, x0:x1].astype(np.float32)
    lum = lin @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    lum = lum / np.percentile(lum[a > 0], white) * 0.92
    w = round((x1 - x0) * h / (y1 - y0))
    premul = cv2.resize(lum * a, (w, h), interpolation=cv2.INTER_AREA)
    alpha = cv2.resize(a, (w, h), interpolation=cv2.INTER_AREA)
    lum = np.where(alpha > 0, premul / np.maximum(alpha, 1e-6), 0)
    srgb = to_srgb(lum)
    srgb = np.clip(srgb + 0.35 * (srgb - cv2.GaussianBlur(srgb, (0, 0), 0.8)), 0, 1)
    return np.round(srgb * 255).astype(np.uint8), np.round(alpha * 255).astype(np.uint8)


def render_glyph(h):
    x, y, w, hh = GLYPH_BOX
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x} {y} {w} {hh}">'
           f'<path fill="#fff" d="{GLYPH_PATH}"/></svg>')
    with tempfile.TemporaryDirectory() as tmp:
        src, out = os.path.join(tmp, "g.svg"), os.path.join(tmp, "g.png")
        with open(src, "w") as f:
            f.write(svg)
        subprocess.run(["rsvg-convert", "-h", str(h), src, "-o", out], check=True)
        return np.asarray(Image.open(out).convert("RGBA"))[:, :, 3].copy()


def c_bytes(data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in data[i:i + 16]))
    return "\n".join(lines)


def c_image(name, cf, w, h, bpp, data, comment):
    return f"""/* {comment} */
static const uint8_t {name}_map[] = {{
{c_bytes(data)}
}};

const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = {cf},
    .header.w = {w},
    .header.h = {h},
    .header.stride = {w * bpp},
    .data_size = sizeof({name}_map),
    .data = {name}_map,
}};
"""


def argb(lum, alpha):
    # LVGL's ARGB8888: straight alpha, stored B, G, R, A.
    return np.dstack([lum, lum, lum, alpha]).reshape(-1).tobytes()


HEADER = """/*
 * Generated by tools/airpods-art.py -- edit that and re-run it, not this.
 *
 * rpod_airpods_photo_buds is adapted from "AirPods Modell Pro 1.jpg" by
 * Burkhard Mücke (https://commons.wikimedia.org/wiki/File:AirPods_Modell_Pro_1.jpg),
 * licensed CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/):
 * cut out of its background, made grey and scaled down. That image is
 * itself licensed CC BY-SA 4.0.
 *
 * rpod_airpods_photo_case is from "AirPod pro.jpg" by Jackienyyy
 * (https://commons.wikimedia.org/wiki/File:AirPod_pro.jpg), CC0.
 *
 * rpod_airpods_glyph_pods is MingCute's "airpods-fill"
 * (https://github.com/Richard9394/MingCute), Copyright MingCute,
 * Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0),
 * rasterized.
 */

#include "lvgl.h"

"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "src/ui/airpods_art_data.c"))
    ap.add_argument("--cache", default=os.path.join(ROOT, ".cache/airpods-art"))
    ap.add_argument("--png", metavar="DIR", help="also write the images as PNGs here")
    args = ap.parse_args()

    rgb = fetch(BUDS, args.cache)
    buds = render(rgb, cut_buds(rgb), BUDS_H, 97)
    rgb = fetch(CASE, args.cache)
    case = render(rgb, cut_case(rgb), CASE_H, 99)
    glyph = render_glyph(GLYPH_H)

    out = [HEADER]
    for name, (lum, alpha), what in (
        ("rpod_airpods_photo_buds", buds, "AirPods Pro, the pair: ARGB8888, white."),
        ("rpod_airpods_photo_case", case, "Their case, lid shut: ARGB8888, white."),
    ):
        h, w = lum.shape
        out.append(c_image(name, "LV_COLOR_FORMAT_ARGB8888", w, h, 4, argb(lum, alpha), what))
        out.append("\n")
    h, w = glyph.shape
    out.append(c_image("rpod_airpods_glyph_pods", "LV_COLOR_FORMAT_A8", w, h, 1, glyph.tobytes(),
                       "The pair as a glyph: an A8 mask, drawn in the image recolour."))
    with open(args.output, "w") as f:
        f.write("".join(out))

    if args.png:
        os.makedirs(args.png, exist_ok=True)
        for name, (lum, alpha) in (("buds", buds), ("case", case)):
            Image.fromarray(np.dstack([lum, lum, lum, alpha]), "RGBA").save(
                os.path.join(args.png, name + ".png"))
        Image.fromarray(glyph, "L").save(os.path.join(args.png, "glyph.png"))


if __name__ == "__main__":
    main()

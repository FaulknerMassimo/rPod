"""Readers for the Rockbox "Wii_menu" theme's files (stdlib only).

  - Windows BMPs (1/4/8/24 bit, uncompressed), as RGBA pixel grids. Rockbox
    treats magenta (FF00FF) as transparent, so it comes out with alpha 0.
  - Rockbox's .fnt fonts (version "RB12"): the clock digits come from one.
  - A minimal PNG writer, for the previews.
"""

import struct
import zlib

MAGENTA = (255, 0, 255)


class Image:
    """w x h pixels, each an (r, g, b, a) tuple, row-major from the top."""

    def __init__(self, w, h, px):
        self.w, self.h, self.px = w, h, px

    def crop(self, x, y, w, h):
        rows = [self.px[(y + j) * self.w + x:(y + j) * self.w + x + w] for j in range(h)]
        return Image(w, h, [p for r in rows for p in r])

    def opaque(self):
        return Image(self.w, self.h, [(r, g, b, 255) for r, g, b, _ in self.px])

    def get(self, x, y):
        return self.px[y * self.w + x]


def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack_from("<I", d, 10)[0]
    hsize = struct.unpack_from("<I", d, 14)[0]
    w, h, _planes, bpp, comp = struct.unpack_from("<iiHHI", d, 18)
    if comp != 0 or hsize < 40:
        raise ValueError(f"{path}: compressed or old-style BMP")
    top_down = h < 0
    h = abs(h)
    ncolors = struct.unpack_from("<I", d, 46)[0] or (1 << bpp if bpp <= 8 else 0)
    pal = []
    for i in range(ncolors if bpp <= 8 else 0):
        b, g, r, _ = d[14 + hsize + 4 * i:14 + hsize + 4 * i + 4]
        pal.append((r, g, b))
    stride = (w * bpp + 31) // 32 * 4
    rows = []
    for y in range(h):
        sy = y if top_down else h - 1 - y
        row = d[off + sy * stride:off + sy * stride + stride]
        out = []
        for x in range(w):
            if bpp == 24:
                b, g, r = row[3 * x:3 * x + 3]
                c = (r, g, b)
            else:
                per = 8 // bpp
                v = (row[x // per] >> (8 - bpp - (x % per) * bpp)) & ((1 << bpp) - 1)
                c = pal[v]
            out.append(c + (0 if c == MAGENTA else 255,))
        rows.append(out)
    return Image(w, h, [p for r in rows for p in r])


def read_rb12_glyphs(path, chars):
    """{char: (w, h, ascent, alpha bytes)} from a Rockbox RB12 4-bit font.

    Pixels are stored row-major, two to a byte (low nibble first), 0 = ink and
    15 = background, so coverage is 15 - nibble."""
    d = open(path, "rb").read()
    if d[:4] != b"RB12":
        raise ValueError(f"{path}: not an RB12 font")
    maxw, h, ascent, depth, first, _default, _size, nbits, noff, nw = struct.unpack_from("<HHHHIIIIII", d, 4)
    if depth != 1:
        raise ValueError(f"{path}: depth {depth}, expected 4-bit")
    bm = 36
    offs = struct.unpack_from(f"<{noff}H", d, bm + nbits)
    widths = d[bm + nbits + 2 * noff:bm + nbits + 2 * noff + nw]
    out = {}
    for ch in chars:
        i = ord(ch) - first
        g = d[bm + offs[i]:bm + offs[i + 1]]
        w = widths[i]
        nib = []
        for b in g:
            nib += [b & 15, b >> 4]
        out[ch] = (w, h, ascent, bytes((15 - nib[k]) * 17 for k in range(w * h)))
    return out


def write_png(path, img):
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in img.px[y * img.w:(y + 1) * img.w]) for y in range(img.h))

    def chunk(t, body):
        c = struct.pack(">I", len(body)) + t + body
        return c + struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))

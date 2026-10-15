"""Pixo fonts and bitmaps for a browser: rPod's web UI draws its Classic themes with them.

to_ttf() turns a bitmap font into a TrueType font whose glyphs are the bitmap's pixels as square
outlines (each glyph's ink traced into its outer contours and holes, so pixels that touch merge
cleanly). One bitmap pixel is 1/16 em, for every font: at CSS font-size 16px a glyph is its native
size, at 32px it's the 2x rPod's panel draws. The vertical metrics put the strike's baseline row on
the baseline and its full height in the line box, as the iPod's renderer places it.

bitmap_svg() is a bitmap as SVG: black squares, the two grays as partial opacity, for use as a CSS
mask that any colour can fill.

Both outputs are Apple's art and stay with the rest of out/, never committed (rPod's PLAN §12).
"""
import struct
import time

from . import pixofont

UPM = 1024
PX = UPM // 16  # one bitmap pixel


# --- outlines ------------------------------------------------------------------------------
def _trace(ink):
    """ink: set of (x, y) pixels, y up. -> closed contours of (x, y) corners, clockwise around
    ink and counter-clockwise around holes (TrueType's winding), collinear points dropped."""
    edges = {}  # start corner -> [end corners]; every edge has ink on its right
    for (x, y) in ink:
        if (x, y + 1) not in ink:
            edges.setdefault((x, y + 1), []).append((x + 1, y + 1))  # top, eastward
        if (x + 1, y) not in ink:
            edges.setdefault((x + 1, y + 1), []).append((x + 1, y))  # right, southward
        if (x, y - 1) not in ink:
            edges.setdefault((x + 1, y), []).append((x, y))  # bottom, westward
        if (x - 1, y) not in ink:
            edges.setdefault((x, y), []).append((x, y + 1))  # left, northward
    contours = []
    while edges:
        start = min(edges)
        pts = [start]
        cur, prev_dir = start, None
        while True:
            outs = edges[cur]
            if len(outs) == 1 or prev_dir is None:
                nxt = outs[0]
            else:
                # Two pixels meeting at a corner: turn right, keeping each one's outline its own.
                def turn(p):
                    d = (p[0] - cur[0], p[1] - cur[1])
                    return prev_dir[0] * d[1] - prev_dir[1] * d[0]  # < 0: a right turn
                nxt = min(outs, key=turn)
            outs.remove(nxt)
            if not outs:
                del edges[cur]
            prev_dir = (nxt[0] - cur[0], nxt[1] - cur[1])
            cur = nxt
            if cur == start:
                break
            pts.append(cur)
        contours.append(_simplify(pts))
    return contours


def _simplify(pts):
    out = []
    n = len(pts)
    for i in range(n):
        a, b, c = pts[i - 1], pts[i], pts[(i + 1) % n]
        if (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) != 0:
            out.append(b)
    return out


def _glyf(contours):
    """A simple glyph: header, end points, no instructions, flags, then x and y deltas."""
    pts = [(x * PX, y * PX) for c in contours for (x, y) in c]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    out = struct.pack('>hhhhh', len(contours), min(xs), min(ys), max(xs), max(ys))
    end, ends = -1, []
    for c in contours:
        end += len(c)
        ends.append(end)
    out += struct.pack('>%dH' % len(ends), *ends) + struct.pack('>H', 0)
    flags, xd, yd = bytearray(), b'', b''
    px = py = 0
    for (x, y) in pts:
        fl = 0x01  # on curve
        dx, dy = x - px, y - py
        if dx == 0:
            fl |= 0x10  # x same as before
        else:
            xd += struct.pack('>h', dx)
        if dy == 0:
            fl |= 0x20
        else:
            yd += struct.pack('>h', dy)
        flags.append(fl)
        px, py = x, y
    out += bytes(flags) + xd + yd
    return out, (min(xs), min(ys), max(xs), max(ys)), len(pts)


# --- tables ----------------------------------------------------------------------------------
def _checksum(data):
    data = data + b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xffffffff


def _cmap4(cp_gid):
    """Format 4 over the BMP: one segment per run of consecutive code points (whose glyph ids
    are consecutive too, being numbered in code point order)."""
    cps = sorted(cp for cp in cp_gid if cp < 0xffff)
    segs, i = [], 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1 and cp_gid[cps[j + 1]] == cp_gid[cps[j]] + 1:
            j += 1
        segs.append((cps[i], cps[j], (cp_gid[cps[i]] - cps[i]) & 0xffff))
        i = j + 1
    segs.append((0xffff, 0xffff, 1))
    n = len(segs)
    sr = 2 * (1 << (n.bit_length() - 1))
    body = struct.pack('>HHHH', n * 2, sr, (sr // 2).bit_length() - 1, n * 2 - sr)
    body += struct.pack('>%dH' % n, *[s[1] for s in segs]) + b'\0\0'
    body += struct.pack('>%dH' % n, *[s[0] for s in segs])
    body += struct.pack('>%dH' % n, *[s[2] for s in segs])
    body += struct.pack('>%dH' % n, *([0] * n))
    sub = struct.pack('>HHH', 4, 6 + len(body), 0) + body
    # Unicode BMP and Windows Unicode BMP, both at the one subtable.
    return struct.pack('>HHHHIHHI', 0, 2, 0, 3, 20, 3, 1, 20) + sub


def _name(family, full, ps):
    recs = {0: 'Glyphs (c) Apple. Extracted from iPod 4G firmware 3.1.1 for rPod.',
            1: family, 2: 'Regular', 3: f'{ps};rPod', 4: full, 5: 'Version 1.000', 6: ps}
    strings, records = b'', b''
    for nid in sorted(recs):
        s = recs[nid].encode('utf-16-be')
        records += struct.pack('>HHHHHH', 3, 1, 0x409, nid, len(s), len(strings))
        strings += s
    return struct.pack('>HHH', 0, len(recs), 6 + len(records)) + records + strings


def to_ttf(f, family):
    """-> TrueType bytes for Pixo font f, named `family`."""
    br, H = f['baseline_row'], f['strike_h']
    glyphs = [(None, None)] + list(pixofont._glyphs(f))  # gid 0: .notdef, empty
    glyf, loca, hmtx = b'', [], b''
    cp_gid = {}
    bbox = [0, 0, 0, 0]
    max_pts = max_contours = 0
    adv_max, min_lsb, min_rsb, max_extent, total_adv = 0, 0x7fff, 0x7fff, 0, 0
    for gid, (cp, box) in enumerate(glyphs):
        loca.append(len(glyf))
        if cp is None:
            adv, contours = 8, []  # .notdef
        else:
            cp_gid[cp] = gid
            s, e, adv, xo = box
            ink = set()
            if e > s:
                for r, row in enumerate(pixofont.glyph_rows(f, s, e)):
                    for c, bit in enumerate(row):
                        if bit:
                            ink.add((xo + c, br - r - 1))  # the pixel's bottom-left corner, y up
            contours = _trace(ink) if ink else []
        aw = adv * PX
        if contours:
            data, (x0, y0, x1, y1), npts = _glyf(contours)
            glyf += data + b'\0' * (-len(data) % 4)
            bbox = [min(bbox[0], x0), min(bbox[1], y0), max(bbox[2], x1), max(bbox[3], y1)]
            max_pts, max_contours = max(max_pts, npts), max(max_contours, len(contours))
            min_lsb, min_rsb, max_extent = min(min_lsb, x0), min(min_rsb, aw - x1), max(max_extent, x1)
            lsb = x0
        else:
            lsb = 0
        hmtx += struct.pack('>Hh', aw, lsb)
        adv_max = max(adv_max, aw)
        total_adv += aw
    loca.append(len(glyf))
    ng = len(glyphs)
    if min_lsb == 0x7fff:
        min_lsb = min_rsb = 0

    asc, desc = br * PX, (H - br) * PX
    stamp = int(time.time()) + 2082844800  # LONGDATETIME: seconds since 1904
    head = struct.pack('>IIIIHHqqhhhhHHhhh', 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0x000B, UPM,
                       stamp, stamp, *bbox, 0, 8, 2, 1, 0)
    hhea = struct.pack('>IhhhHhhhhhhhhhhhH', 0x00010000, asc, -desc, 0, adv_max, min_lsb, min_rsb,
                       max_extent, 1, 0, 0, 0, 0, 0, 0, 0, ng)
    maxp = struct.pack('>IHHHHHHHHHHHHHH', 0x00010000, ng, max_pts, max_contours, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)
    cps = [cp for cp in cp_gid if cp < 0xffff]
    x_box = pixofont.glyph_box(f, ord('x'))
    cap = br * PX
    xh = cap // 2
    if x_box:
        rows = pixofont.glyph_rows(f, x_box[0], x_box[1])
        ink_rows = [r for r, row in enumerate(rows) if any(row)]
        if ink_rows:
            xh = (br - ink_rows[0]) * PX
    # Each font is a family of its own, bold or not (the page names them by face): Regular.
    os2 = struct.pack('>HhHHH', 4, total_adv // ng, 400, 5, 0)
    os2 += struct.pack('>hhhhhhhh', 10 * PX, 10 * PX, 0, PX, 10 * PX, 10 * PX, 0, 5 * PX)
    os2 += struct.pack('>hh', PX, xh // 2 + PX // 2)
    os2 += struct.pack('>h', 0) + b'\0' * 10
    os2 += struct.pack('>IIII', 0x00000003, 0, 0, 0)  # Basic Latin, Latin-1 Supplement
    os2 += b'rPod'
    os2 += struct.pack('>HHH', 0x00C0, min(cps), max(cps))  # REGULAR | USE_TYPO_METRICS
    os2 += struct.pack('>hhhHH', asc, -desc, 0, asc, desc)
    os2 += struct.pack('>II', 1, 0)  # Latin 1
    os2 += struct.pack('>hhHHH', xh, cap, 0, 32, 1)
    post = struct.pack('>IIhhIIIII', 0x00030000, 0, -PX, PX, 0, 0, 0, 0, 0)
    ps = family.replace(' ', '') + '-Regular'
    tables = {
        b'OS/2': os2, b'cmap': _cmap4(cp_gid), b'glyf': glyf, b'head': head, b'hhea': hhea,
        b'hmtx': hmtx, b'loca': struct.pack('>%dI' % len(loca), *loca), b'maxp': maxp,
        b'name': _name(family, family, ps), b'post': post,
    }
    tags = sorted(tables)
    n = len(tags)
    sr = 16 * (1 << (n.bit_length() - 1))
    out = struct.pack('>IHHHH', 0x00010000, n, sr, (sr // 16).bit_length() - 1, n * 16 - sr)
    offset = 12 + 16 * n
    directory, body = b'', b''
    for tag in tags:
        data = tables[tag]
        directory += struct.pack('>4sIII', tag, _checksum(data), offset + len(body), len(data))
        body += data + b'\0' * (-len(data) % 4)
    font = bytearray(out + directory + body)
    head_at = offset + sum(len(tables[t]) + (-len(tables[t]) % 4) for t in tags[:tags.index(b'head')])
    struct.pack_into('>I', font, head_at + 8, (0xB1B0AFBA - _checksum(bytes(font))) & 0xffffffff)
    return bytes(font)


# --- bitmaps -----------------------------------------------------------------------------
def bitmap_svg(bm):
    """-> SVG text: the bitmap's pixels as black runs; its grays (levels 1-2) as 1/3 and 2/3."""
    rects = []
    for y, row in enumerate(bm['px']):
        x = 0
        while x < bm['w']:
            v = row[x]
            if v == 0:
                x += 1
                continue
            x0 = x
            while x < bm['w'] and row[x] == v:
                x += 1
            op = '' if v == 3 else f' fill-opacity="{v / 3:.3f}"'
            rects.append(f'<rect x="{x0}" y="{y}" width="{x - x0}" height="1"{op}/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {bm["w"]} {bm["h"]}" '
            f'width="{bm["w"]}" height="{bm["h"]}" shape-rendering="crispEdges">{"".join(rects)}</svg>\n')

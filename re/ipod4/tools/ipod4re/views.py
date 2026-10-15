"""View resources: decoding, layout, and a reference 160x128 renderer.

View resource = u32 count, then elements { u32 size; body[size] }:
  body: u32 class, char refType[4] (byte-reversed: 'None','Str ','BMap','Cntl','StSt'),
        u32 refId, s32 owner (class whose resources resolve refId, or -1/8 for system),
        u32 aux, u32 flags, 4 x constraint {u32 ref, u32 kind, s32 value}, s32 extra[...]
  constraint slots: 0 top, 1 left, 2 bottom, 3 right.
  kind with bit31 set -> relative to parent:
      0x01 absolute offset from parent origin   0x02 offset from parent's same edge
      0x04 width = value (slot 1/3)             0x08 height = value (slot 0/2)
      0x10 flag: auto-size to content (value = default / preferred size)
      0x22 center in parent (size from the opposite slot)
  kind without bit31 -> relative to a sibling (ref 0 = previous sibling, else sibling id):
      0x100 its top, 0x200 its left, 0x300 its bottom, 0x400 its right (+ value)
      ref 1 with kind 1/2 -> offset from the parent's centre line.
  Text elements: extra[0] = TypeId << 16 | staticStringId (16-bit signed).
  Animated images (0x1300): extra = [frameMs, frameHeight, loop].
  Flags: 0x800 visible by default; 0x8000 on a plain view = solid fill (title-bar rule).

Class ids (is-kind-of chains recovered from the vtables at osos+0x20000..0x29000):
"""
import struct

from .rsrc import sid16
from . import pixofont

CLASS_NAMES = {
    0x1080: 'View', 0x1100: 'Group', 0x1180: 'ScrollList', 0x1200: 'Window', 0x1280: 'Image',
    0x1300: 'AnimatedImage', 0x1400: 'Control', 0x1580: 'Text', 0x1700: 'MenuBase', 0x1780: 'MenuRow',
    0x1800: 'Menu', 0x1980: 'Indicator', 0x1F00: 'ScrollBar', 0x2F00: 'Deck', 0x3080: 'Slider',
    0x3180: 'MarqueeText', 0x4800: 'RatingControl', 0x4980: 'Battery',
}

# 4-level LCD palette. Firmware colour constants (init @0x1C8FC): white FFFFFF, AAAAAA, 7F7F7F, 555555, black.
GRAY_RGB = [(255, 255, 255), (170, 170, 170), (85, 85, 85), (0, 0, 0)]
W, H = 160, 128


def parse_view(b):
    n = struct.unpack_from('<I', b, 0)[0]
    p, els = 4, []
    for _ in range(n):
        size = struct.unpack_from('<I', b, p)[0]
        body = b[p + 4:p + 4 + size]
        p += 4 + size
        cls, fc, rid, owner, aux, flags = struct.unpack_from('<I4sIiII', body, 0)
        cons = [struct.unpack_from('<IIi', body, 24 + k * 12) for k in range(4)]
        extra = list(struct.unpack_from('<%di' % ((size - 72) // 4), body, 72)) if size > 72 else []
        els.append(dict(cls=cls, cls_name=CLASS_NAMES.get(cls, f'cls_{cls:04x}'), ref_type=fc[::-1].decode('latin1'),
                        ref_id=sid16(rid) if rid <= 0xffff else rid, owner=owner, aux=aux, flags=flags,
                        cons=cons, extra=extra))
    return els


def describe_constraint(slot, c):
    ref, kind, v = c
    if kind & 0x80000000:
        k = kind & 0xff
        base = {0x01: 'abs', 0x02: 'edge', 0x04: 'width', 0x08: 'height', 0x22: 'center'}.get(k & ~0x10, f'k{k:x}')
        return f"{'auto-' if k & 0x10 else ''}{base} {v}"
    if ref == 1 and kind in (1, 2):
        return f"parent-center {v:+d}"
    side = {1: 'top', 2: 'left', 3: 'bottom', 4: 'right'}.get(kind >> 8, f'k{kind:x}')
    who = 'prev' if ref == 0 else f'#{sid16(ref)}'
    return f"{who}.{side} {v:+d}"


class Rect:
    __slots__ = ('t', 'l', 'b', 'r')

    def __init__(self, t, l, b, r):
        self.t, self.l, self.b, self.r = t, l, b, r

    def side(self, s):
        return (None, self.t, self.l, self.b, self.r)[s]

    def __repr__(self):
        return f"Rect(x={self.l},y={self.t},w={self.r - self.l},h={self.b - self.t})"


def _edge(slot, c, P, prev, byid):
    ref, kind, v = c
    vertical = slot in (0, 2)
    if kind & 0x80000000:
        k = kind & 0xff
        if k & 0x10:
            return ('auto', v)
        if k == 0x22:
            return ('center', v)
        if k == 0x01:
            return ('pos', (P.t if vertical else P.l) + v)
        if k == 0x02:
            return ('pos', P.side(slot + 1) + v)
        if k in (0x04, 0x08):
            return ('size', v)
        return ('pos', P.side(slot + 1))
    if ref == 1 and kind in (1, 2):
        return ('pos', ((P.t + P.b) // 2 if vertical else (P.l + P.r) // 2) + v)
    S = prev if ref == 0 else byid.get(sid16(ref))
    if S is None:
        return ('pos', P.side(slot + 1))
    return ('pos', S.side(kind >> 8) + v)


def resolve(el, P, prev, byid, measure):
    out = []
    for a, b, lo, hi in ((0, 2, P.t, P.b), (1, 3, P.l, P.r)):
        A = _edge(a, el['cons'][a], P, prev, byid)
        B = _edge(b, el['cons'][b], P, prev, byid)
        size = None
        for X in (A, B):
            if X[0] == 'size':
                size = X[1]
            elif X[0] == 'auto':
                m = measure(el, a == 0)
                size = X[1] if m is None else m
        if A[0] == 'pos' and B[0] == 'pos':
            s, e = A[1], B[1]
        elif A[0] == 'pos':
            s, e = A[1], A[1] + (size or 0)
        elif B[0] == 'pos':
            s, e = B[1] - (size or 0), B[1]
        elif 'center' in (A[0], B[0]):
            c = A if A[0] == 'center' else B
            s = lo + ((hi - lo) - (size or 0)) // 2 + c[1]
            e = s + (size or 0)
        else:
            s, e = lo, lo + (size or 0)
        out.append((s, e))
    (t, b), (l, r) = out
    return Rect(t, l, b, r)


# --- canvas --------------------------------------------------------------------
class Canvas:
    def __init__(self, w=W, h=H):
        self.w, self.h = w, h
        self.px = [[0] * w for _ in range(h)]

    def set(self, x, y, v, clip=None):
        if clip and not (clip.l <= x < clip.r and clip.t <= y < clip.b):
            return
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = v

    def fill(self, R, v):
        for y in range(max(0, R.t), min(self.h, R.b)):
            for x in range(max(0, R.l), min(self.w, R.r)):
                self.px[y][x] = v

    def invert(self, R):
        for y in range(max(0, R.t), min(self.h, R.b)):
            for x in range(max(0, R.l), min(self.w, R.r)):
                self.px[y][x] = 3 - self.px[y][x]

    def line(self, x0, y0, x1, y1, v=3):
        """Inclusive endpoints, axis-aligned (all firmware DrawLine calls seen are)."""
        for y in range(min(y0, y1), max(y0, y1) + 1):
            for x in range(min(x0, x1), max(x0, x1) + 1):
                self.set(x, y, v)

    def bitmap(self, bm, x, y, clip=None, src_y=0, h=None):
        h = bm['h'] if h is None else h
        for yy in range(h):
            for xx, v in enumerate(bm['px'][src_y + yy]):
                if v:
                    self.set(x + xx, y + yy, v, clip)

    def text(self, font, s, R, align='left', ellipsis=True):
        """Strike vertically centred in R; truncated with U+2026 when wider than R."""
        width = pixofont.text_width(font, s)
        if ellipsis and width > R.r - R.l and s:
            ell = pixofont.text_width(font, '…')
            while s and pixofont.text_width(font, s) + ell > R.r - R.l:
                s = s[:-1]
            s = s.rstrip() + '…'
            width = pixofont.text_width(font, s)
        x = {'left': R.l, 'center': R.l + (R.r - R.l - width) // 2, 'right': R.r - width}[align]
        y = R.t + ((R.b - R.t) - font['strike_h']) // 2
        for ch in s:
            g = pixofont.glyph_box(font, ord(ch))
            if not g:
                continue
            gs, ge, adv, xo = g
            for yy, row in enumerate(pixofont.glyph_rows(font, gs, ge)):
                for xx, bit in enumerate(row):
                    if bit:
                        self.set(x + xo + xx, y + yy, 3, R)
            x += adv

    def to_image(self, scale=1, tint=None):
        from PIL import Image
        pal = tint or GRAY_RGB
        im = Image.new('RGB', (self.w, self.h))
        im.putdata([pal[v] for row in self.px for v in row])
        return im.resize((self.w * scale, self.h * scale), Image.NEAREST) if scale != 1 else im


# --- scene renderer --------------------------------------------------------------
class Renderer:
    """Draws a View tree with the semantics recovered from the Draw methods:
       ScrollBar  (0x5A6E0/0x3C49C), Slider (0x5AB30), Image/Text (vtable slot 25)."""

    ROW_H = 18

    def __init__(self, views, bitmaps, types, fonts, strings, menus):
        self.views, self.bitmaps, self.types, self.fonts = views, bitmaps, types, fonts
        self.strings, self.menus = strings, menus
        self.font_by = {(f['name'], f['size'], f['bold']): f for f in fonts}
        self.log = []

    def font_for(self, type_id):
        t = self.types.get(type_id)
        default = self.font_by[('Chicago', 12, False)]
        if not t:
            return default, 'left'
        fam, size, bold = t['family'], t['size'], t['bold']
        f = self.font_by.get((fam, size, bold)) or self.font_by.get((fam, size, not bold))
        if f is None:
            # Charcoal/Geneva/Techno/Verdana have Type records but no font data in this build;
            # pick the nearest size of the same family, else Chicago 12.
            same = [x for x in self.fonts if x['name'] == fam]
            f = min(same, key=lambda x: (abs(x['size'] - size), x['bold'] != bold)) if same else default
        return f, t['align']

    def text_of(self, el, ctx):
        key = el['ref_id']
        if key in ctx.get('text', {}):
            return ctx['text'][key]
        static = sid16(el['extra'][0]) if el['extra'] else 0
        if static and static in self.strings:
            return self.strings[static]
        if key in self.strings and el['owner'] in (-1, 8):
            return self.strings[key]
        return ctx.get('placeholder', '')

    def measure(self, ctx):
        def m(el, vertical):
            if el['ref_type'] in ('Str ', 'StSt') and el['extra']:
                f, _ = self.font_for((el['extra'][0] >> 16) & 0xffff)
                return f['strike_h'] if vertical else pixofont.text_width(f, self.text_of(el, ctx))
            if el['ref_type'] == 'BMap':
                bm = self.bitmaps.get(ctx.get('bmap', {}).get(el['ref_id'], el['ref_id']))
                if bm:
                    if el['cls'] == 0x1300 and len(el['extra']) > 1:
                        return el['extra'][1] if vertical else bm['w']
                    return bm['h'] if vertical else bm['w']
            if el['cls'] == 0x1F00 and not vertical:
                return 10 if ctx.get('scroll_needed') else 0      # preferred width captured at first layout
            return None
        return m

    def render_view(self, cv, view_id, R, ctx, depth=0):
        els = self.views.get(view_id)
        if els is None:
            return
        prev, byid, placed = None, {}, []
        for el in els:
            rect = resolve(el, R, prev, byid, self.measure(ctx))
            prev = rect
            byid[el['ref_id'] if isinstance(el['ref_id'], int) else 0] = rect
            byid[sid16(el['aux'])] = byid.get(sid16(el['aux']), rect)
            placed.append((el, rect))
        for el, rect in placed:
            vis = ctx.get('show', {}).get(el['ref_id'])
            if vis is None:
                vis = bool(el['flags'] & 0x800)
            if not vis:
                continue
            self.draw(cv, el, rect, ctx, depth)

    def draw(self, cv, el, R, ctx, depth):
        self.log.append((depth, el['cls_name'], el['ref_type'], el['ref_id'], R))
        c = el['cls']
        if c == 0x1080:
            if el['flags'] & 0x8000:
                cv.fill(R, 3)
        elif c in (0x1580, 0x3180):
            f, align = self.font_for((el['extra'][0] >> 16) & 0xffff)
            cv.text(f, self.text_of(el, ctx), R, align)
        elif c in (0x1280, 0x1300):
            bm = self.bitmaps.get(ctx.get('bmap', {}).get(el['ref_id'], el['ref_id']))
            if bm:
                fh = el['extra'][1] if c == 0x1300 and len(el['extra']) > 1 else bm['h']
                cv.bitmap(bm, R.l, R.t, R, 0, min(fh, bm['h']))
        elif c == 0x4980:
            bm = self.bitmaps.get(ctx.get('battery', 0x25D8))
            if bm:
                cv.bitmap(bm, R.l, R.t, R)
        elif c == 0x1F00:
            self.draw_scrollbar(cv, R, ctx)
        elif c in (0x3080,):
            self.draw_slider(cv, R, ctx.get('slider', {}).get(el['ref_id'], 0.35), rounded=True)
        elif c == 0x1180:
            self.draw_list(cv, el, R, ctx, depth)
        elif c == 0x2F00:
            card = ctx.get('deck', {}).get(el['ref_id'])
            inner = self.views.get(el['ref_id'], [])
            if card is None and inner:
                card = inner[0]['ref_id']
            for ch in inner:
                if ch['ref_id'] == card:
                    r2 = resolve(ch, R, None, {}, self.measure(ctx))
                    self.draw(cv, ch, r2, dict(ctx, show={**ctx.get('show', {}), card: True}), depth + 1)
        elif el['ref_type'] == 'None' and el['ref_id'] in self.views:
            self.render_view(cv, el['ref_id'], R, ctx, depth + 1)

    def draw_scrollbar(self, cv, R, ctx):
        if not ctx.get('scroll_needed') or R.r - R.l <= 0:
            return
        cv.fill(R, 0)
        cv.line(R.l + 1, R.t, R.l + 1, R.b - 1)          # left rule  (x = left+1)
        cv.line(R.l + 1, R.b - 1, R.r - 1, R.b - 1)      # bottom rule
        cv.line(R.r - 1, R.t, R.r - 1, R.b - 1)          # right rule
        n, first, vis = ctx['list_count'], ctx.get('list_top', 0), ctx['list_visible']
        content, visible = n * self.ROW_H, vis * self.ROW_H
        top, bot = R.t + 1, R.b - 2
        track = bot - top
        thumb = max(3, visible * track // content)
        thumb = min(thumb, track)
        free = track - thumb
        off = (first * self.ROW_H) * free // max(1, content - visible)
        cv.fill(Rect(top + off, R.l + 3, top + off + thumb, R.r - 2), 3)

    def draw_slider(self, cv, R, frac, rounded=True):
        w, h = R.r - R.l, R.b - R.t
        fill_r = R.l + 1 + int((w - 2) * frac)
        cv.fill(Rect(R.t + 1, R.l + 1, R.b - 1, fill_r), 2)   # elapsed: #555555
        x0, y0 = R.l, R.t
        L = lambda a, b, c, d: cv.line(x0 + a, y0 + b, x0 + c, y0 + d)
        if rounded:
            L(0, 2, 0, h - 3); L(1, 1, 1, 1); L(1, h - 2, 1, h - 2)
            L(w - 1, 2, w - 1, h - 3); L(w - 2, 1, w - 2, 1); L(w - 2, h - 2, w - 2, h - 2)
            L(2, 0, w - 3, 0); L(2, h - 1, w - 3, h - 1)
        else:
            L(0, 0, w - 1, 0); L(0, h - 1, w - 1, h - 1); L(0, 0, 0, h - 1); L(w - 1, 0, w - 1, h - 1)

    def draw_list(self, cv, el, R, ctx, depth):
        menu_view = self.views.get(el['ref_id'], [])
        menu_el = next((m for m in menu_view if m['cls'] == 0x1800), None)
        if not menu_el:
            return
        items = ctx.get('items')
        if items is None:
            items = [dict(label=self.strings.get(it['label'], '?'),
                          accessory=it['accessory'] if it['accessory_kind'] == 8 else None,
                          value=None) for it in self.menus.get(menu_el['ref_id'], [])]
        template = self.views.get(menu_el['ref_id'], [])
        row_el = next((m for m in template if m['cls'] == 0x1780), None)
        row_view = row_el['ref_id'] if row_el else -184
        first = ctx.get('list_top', 0)
        sel = ctx.get('selected', 0)
        for i, it in enumerate(items[first:first + ctx.get('list_visible', 6)]):
            rr = Rect(R.t + i * self.ROW_H, R.l, R.t + (i + 1) * self.ROW_H, R.r)
            rctx = dict(ctx, text={0x301: it['label'], 0x304: it.get('value') or ''},
                        bmap={0x305: it.get('accessory')}, show={0x301: True, 0x304: bool(it.get('value')), 0x305: bool(it.get('accessory'))})
            self.render_view(cv, row_view, rr, rctx, depth + 1)
            if first + i == sel:
                cv.invert(rr)

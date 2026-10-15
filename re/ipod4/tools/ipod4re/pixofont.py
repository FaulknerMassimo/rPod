"""Pixo bitmap fonts.

The OS keeps two font tables of (charmapPtr, fontDataPtr) pairs at osos+0x331518
(13 Pixo sizes) and osos+0x331A08 (AppleGothic, AppleLiGothicMedium, Chicago,
Espy Sans x3, Hei, Osaka -- CJK faces split into Unicode-block sub-fonts).

charmap (A):  u16 ver(4) u16 ? u16 firstChar u16 lastChar u16 defaultGlyph u16 nGlyphs
              u32 nRanges, nRanges x { u16 firstUnicode, u16 firstGlyph, u32 count }
fontdata (B): u16 4, char name[64]
              @+0x42: u16 0, u16 size, u16 0, u8 bold, u8 ?, u16 nGlyphs, u16 0,
                      u16 lineHeight, u16 ascent, u16 descent, u16 leading, u16 0,
                      u32 indexOff, u32 locOff, u32 strikeOff      (offsets relative to B)
              index:  nGlyphs x u16  -> glyph g (1-based) uses loc[index[g-1]]
              loc:    n x { u16 bitStart, u16 bitEnd, u16 advance, s16 xOffset }
              strike: u16 top,left,bottom,right, u16 rowBytes, u16 depth(1), u32 flags, u32 size, bits
A glyph image is columns [bitStart, bitEnd) of the 1-bpp strike, drawn at pen+xOffset;
the pen then advances by `advance`. Fonts with an empty loc table are fixed-cell (CJK).
"""
import struct

FONT_TABLES = ((0x331518, 0x331580), (0x331A08, 0x331B60))


def parse_font(d, A, B):
    _ver, _unk, first, last, defg, _ng, nr = struct.unpack_from('<HHHHHHI', d, A)
    ranges = [struct.unpack_from('<HHI', d, A + 16 + i * 8) for i in range(nr)]
    name = d[B + 2:B + 66].split(b'\0')[0].decode()
    (_z0, size, _z1, bold, _f2, ng, _z2, lineh, ascent, descent, lead, _z3,
     ioff, loff, soff) = struct.unpack_from('<HHHBBHHHHHHHIII', d, B + 0x42)
    index = struct.unpack_from('<%dH' % ng, d, B + ioff)
    loc = [struct.unpack_from('<HHHh', d, B + loff + i * 8) for i in range((soff - loff) // 8)]
    st, sl, sb, sr, rb, depth = struct.unpack_from('<HHHHHH', d, B + soff)
    _sflags, ssize = struct.unpack_from('<II', d, B + soff + 12)
    cmap = {}
    for u, g, c in ranges:
        for k in range(c):
            cmap[u + k] = g + k
    f = dict(name=name, size=size, bold=bool(bold), nglyphs=ng, line_height=lineh, ascent=ascent,
             descent=descent, leading=lead, first=first, last=last, default_glyph=defg, cmap=cmap,
             index=index, loc=loc, strike_w=sr - sl, strike_h=sb - st, rowbytes=rb,
             strike=d[B + soff + 20:B + soff + 20 + ssize], charmap_addr=A, data_addr=B)
    f['fixed_cell'] = (f['strike_w'] // ng) if not loc else 0
    f['baseline_row'] = _baseline_row(f)
    return f


def all_fonts(d):
    out = []
    for start, end in FONT_TABLES:
        for p in range(start, end, 8):
            a, b = struct.unpack_from('<II', d, p)
            out.append(parse_font(d, a, b))
    return out


def glyph_box(f, codepoint):
    """-> (bitStart, bitEnd, advance, xOffset) or None."""
    g = f['cmap'].get(codepoint)
    if g is None or g < 1 or g > len(f['index']):
        return None
    li = f['index'][g - 1]
    if f['fixed_cell']:
        cw = f['fixed_cell']
        return li * cw, li * cw + cw, cw, 0
    if li >= len(f['loc']):
        return None
    return f['loc'][li]


def glyph_rows(f, s, e):
    rows = []
    for y in range(f['strike_h']):
        base = y * f['rowbytes']
        rows.append([(f['strike'][base + (x >> 3)] >> (7 - (x & 7))) & 1 for x in range(s, e)])
    return rows


def _baseline_row(f):
    """Strike row the baseline sits on, measured from 'H' ink (Chicago 12: row 12 although ascent=13)."""
    for ch in 'H0Ix':
        b = glyph_box(f, ord(ch))
        if b and b[1] > b[0]:
            ink = [y for y, r in enumerate(glyph_rows(f, b[0], b[1])) if any(r)]
            if ink:
                return ink[-1] + 1
    return f['ascent']


def text_width(f, text):
    return sum((glyph_box(f, ord(c)) or (0, 0, 0, 0))[2] for c in text)


def font_key(f):
    return f"{f['name']}|{f['size']}|{'b' if f['bold'] else 'r'}"


def slug(f):
    return (f"{f['name']}_{f['size']}{'_bold' if f['bold'] else ''}").lower().replace(' ', '_')


# --- export ------------------------------------------------------------------
def _glyphs(f):
    for cp in sorted(f['cmap']):
        b = glyph_box(f, cp)
        if b:
            yield cp, b


def to_bdf(f):
    br, H = f['baseline_row'], f['strike_h']
    glyphs = list(_glyphs(f))
    maxw = max([max(1, e - s) for _, (s, e, a, x) in glyphs] + [1])
    out = ['STARTFONT 2.1', f"FONT -Apple-{f['name'].replace(' ', '')}-{'Bold' if f['bold'] else 'Medium'}-R-Normal--{f['size']}-{f['size'] * 10}-72-72-P-0-ISO10646-1",
           f"SIZE {f['size']} 72 72", f"FONTBOUNDINGBOX {maxw} {H} 0 {br - H}",
           'STARTPROPERTIES 6', f"FONT_ASCENT {br}", f"FONT_DESCENT {H - br}", f"PIXEL_SIZE {f['size']}",
           f"FAMILY_NAME \"{f['name']}\"", f"WEIGHT_NAME \"{'Bold' if f['bold'] else 'Medium'}\"",
           'COMMENT "Extracted from iPod 4G firmware 3.1.1 (osos). Apple copyright."', 'ENDPROPERTIES',
           f"CHARS {len(glyphs)}"]
    for cp, (s, e, adv, xo) in glyphs:
        w = e - s
        rows = glyph_rows(f, s, e) if w else []
        out += [f"STARTCHAR U+{cp:04X}", f"ENCODING {cp}", f"SWIDTH {adv * 1000 // max(1, f['size'])} 0",
                f"DWIDTH {adv} 0", f"BBX {w} {H if w else 0} {xo} {br - H if w else 0}", 'BITMAP']
        nbytes = (w + 7) // 8
        for r in rows:
            v = 0
            for x, bit in enumerate(r):
                v |= bit << (nbytes * 8 - 1 - x)
            out.append(f"{v:0{nbytes * 2}X}")
        out.append('ENDCHAR')
    out.append('ENDFONT')
    return '\n'.join(out) + '\n'


def to_lvgl_c(f, scale=1, symbol=None):
    """LVGL 9 lv_font_fmt_txt source, 1 bpp, rows cropped to ink, pixel-doubled when scale=2."""
    symbol = symbol or f"ipod_font_{slug(f)}" + (f"_x{scale}" if scale != 1 else '')
    br, H = f['baseline_row'], f['strike_h']
    line_h = max(f['line_height'], H)
    glyphs = list(_glyphs(f))
    bitmap, dscs = [], ['    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */']
    for cp, (s, e, adv, xo) in glyphs:
        rows = glyph_rows(f, s, e) if e > s else []
        ink = [y for y, r in enumerate(rows) if any(r)]
        if not ink:
            dscs.append(f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {adv * scale * 16}, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}} /* U+{cp:04X} */")
            continue
        t, b = ink[0], ink[-1]
        bits = []
        for y in range(t, b + 1):
            row = [bit for bit in rows[y] for _ in range(scale)]
            bits.extend(row * scale)
        idx = len(bitmap)
        for i in range(0, len(bits), 8):
            chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
            bitmap.append(sum(bit << (7 - k) for k, bit in enumerate(chunk)))
        box_w, box_h = (e - s) * scale, (b - t + 1) * scale
        ofs_y = (br - (b + 1)) * scale
        ch = chr(cp) if 0x20 < cp < 0x7f and chr(cp) not in '\\*/' else ''
        dscs.append(f"    {{.bitmap_index = {idx}, .adv_w = {adv * scale * 16}, .box_w = {box_w}, .box_h = {box_h}, .ofs_x = {xo * scale}, .ofs_y = {ofs_y}}} /* U+{cp:04X} {ch} */")
    # contiguous unicode runs -> FORMAT0_TINY cmaps (glyph ids are consecutive in file order)
    cmaps, gid = [], 1
    cps = [cp for cp, _ in glyphs]
    i = 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1:
            j += 1
        cmaps.append(f"    {{.range_start = {cps[i]}, .range_length = {j - i + 1}, .glyph_id_start = {gid}, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}}")
        gid += j - i + 1
        i = j + 1
    hexrows = [', '.join(f'0x{v:02x}' for v in bitmap[k:k + 16]) for k in range(0, len(bitmap), 16)]
    return f"""/*
 * {f['name']} {f['size']}{' Bold' if f['bold'] else ''} -- iPod 4G firmware 3.1.1 bitmap font, {'pixel-doubled x%d' % scale if scale != 1 else 'native size'}.
 * Generated by re/tools/extract.py from osos (font data @0x{f['data_addr']:06x}). Glyph art (c) Apple.
 * Strike height {H}px, baseline on strike row {br}; line_height keeps strike row 0 at the line top,
 * which is how the Pixo renderer places it.
 */
#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
    {(','+chr(10)+'    ').join(hexrows) if hexrows else '0x00'}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{(','+chr(10)).join(dscs)}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
{(','+chr(10)).join(cmaps)}
}};

static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = {len(cmaps)},
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t {symbol} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_h * scale},
    .base_line = {(line_h - br) * scale},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = {-1 * scale},
    .underline_thickness = {scale},
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
}};
""", symbol

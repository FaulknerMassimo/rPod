"""Pixo resource files embedded in osos.

Layout (all little endian):
  +0   u32 size (unused by the language files)
  +4   u32 version (3)
  +8   u32 dataStart         } offsets below are relative to file+4
  +12  u32 typeCount
  +16  typeCount x { char fourcc[4] (byte-reversed), u32 count, u32 flags, u32 indexOffset }
  index entries: count x { s32 id, u32 dataOffset (rel. dataStart), u32 size }

The main file is at osos+0x1A5078 (BMap, LDTm, Menu, Str, TCmd, Type, View);
per-language files (LDTm, Str, [Type]) sit between 0x175000 and 0x1A2000.
"""
import re
import struct

MAIN_RESOURCE_FILE = 0x1A5078


class ResourceFile:
    def __init__(self, data, base):
        self.data, self.base = data, base
        rel = base + 4
        self.size, self.version, data_start, ntypes = struct.unpack_from('<IIII', data, base)
        self.types = {}
        for i in range(ntypes):
            fc, cnt, flags, off = struct.unpack_from('<4sIII', data, base + 16 + i * 16)
            fc = fc[::-1].decode('latin1')
            order, ents = [], {}
            for j in range(cnt):
                rid, roff, rsz = struct.unpack_from('<iII', data, rel + off + j * 12)
                ents[rid] = (rel + data_start + roff, rsz)
                order.append(rid)
            self.types[fc] = dict(flags=flags, entries=ents, order=order)

    def ids(self, fourcc):
        return list(self.types.get(fourcc, {}).get('order', []))

    def has(self, fourcc, rid):
        return rid in self.types.get(fourcc, {}).get('entries', {})

    def offset(self, fourcc, rid):
        return self.types[fourcc]['entries'][rid]

    def get(self, fourcc, rid):
        off, size = self.offset(fourcc, rid)
        return self.data[off:off + size]

    def string(self, rid):
        return self.get('Str ', rid).split(b'\0')[0].decode('utf-8', 'replace')


def language_files(data):
    """Locate per-language resource files: version 3 header whose first type is LDTm."""
    out = []
    for m in re.finditer(b'mTDL', data):
        base = m.start() - 16
        if base < MAIN_RESOURCE_FILE and struct.unpack_from('<I', data, base + 4)[0] == 3:
            out.append(ResourceFile(data, base))
    return out


def sid16(v):
    """IDs are 16-bit signed in View/Menu records (0xff48 == -184)."""
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


# --- BMap -----------------------------------------------------------------
def parse_bmap(b):
    """BMap: Rect{top,left,bottom,right} (s32), u16 rowBytes, u16 depth (1|2), u32 flags, u32 dataSize, pixels.
    Pixels are MSB-first; depth 2: 0=white 1=light 2=dark 3=black; depth 1: 1=black."""
    top, left, bottom, right = struct.unpack_from('<iiii', b, 0)
    rowbytes, depth = struct.unpack_from('<HH', b, 16)
    flags, dsz = struct.unpack_from('<II', b, 20)
    w, h = right - left, bottom - top
    data = b[28:28 + dsz]
    px = []
    for y in range(h):
        row = data[y * rowbytes:(y + 1) * rowbytes]
        if depth == 2:
            px.append([(row[x >> 2] >> (6 - 2 * (x & 3))) & 3 for x in range(w)])
        else:
            px.append([((row[x >> 3] >> (7 - (x & 7))) & 1) * 3 for x in range(w)])
    return dict(w=w, h=h, depth=depth, rowbytes=rowbytes, flags=flags, px=px)


# --- Type (text style) ------------------------------------------------------
ALIGN = {0: 'left', 1: 'center', 2: 'right'}


def parse_type(b, strings):
    family, size, _z, style = struct.unpack_from('<IIII', b, 0)
    return dict(family_id=family, family=strings.get(family, '?'), size=size,
                bold=bool(style & 0xff), align=ALIGN.get((style >> 8) & 0xff, 'left'))


# --- Menu -------------------------------------------------------------------
def parse_menu(b):
    """u32 count, then 40-byte items:
       u32 recSize(0x24) u32 flags u32 itemClass u32 command u32 submenu
       u32 (shortcutKey<<16 | labelStr) u32 accessoryId u32 accessoryKind u32 titleStr u32 extra
    accessoryKind: 8 = BMap id (0x25AE '>' arrow), 0x5001 = none, other = owner class whose
    resource supplies a value string (e.g. 0x2780 Settings, 0x4580 Clock)."""
    n = struct.unpack_from('<I', b, 0)[0]
    items = []
    for i in range(n):
        f = struct.unpack_from('<10I', b, 4 + i * 40)
        items.append(dict(flags=f[1], item_class=f[2], command=f[3], submenu=sid16(f[4]) if f[4] else 0,
                          label=sid16(f[5]), shortcut=(f[5] >> 16) & 0xffff,
                          accessory=sid16(f[6]) if f[6] else 0, accessory_kind=f[7],
                          title=sid16(f[8]), extra=f[9]))
    return items

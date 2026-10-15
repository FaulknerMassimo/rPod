#!/usr/bin/env python3
"""Extract the iPod 4G firmware 3.1.1 UI into reusable assets + data.

usage: python3 -I tools/extract.py [--no-png] <iPod_4.3.1.1.ipsw> <outdir>
needs: Pillow (PNG output), unless --no-png -- which skips the bitmap PNGs,
the font specimen and the mockups, and is all rPod's build (lvgl/) needs
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ipod4re import firmware, rsrc, pixofont, views  # noqa: E402
from ipod4re.rsrc import sid16  # noqa: E402

LATIN_FONTS = ('Pixo', 'Chicago', 'Espy Sans')
LANG_BY_SETTINGS = {'Indstillinger': 'da', 'Einstellungen': 'de', 'Ajustes': 'es', 'Asetukset': 'fi',
                    'Réglages': 'fr', 'Impostazioni': 'it', 'Instellingen': 'nl', 'Innstillinger': 'no',
                    'Inställningar': 'sv', '설정': 'ko', '设置': 'zh-Hans'}
BITMAP_NAMES = {
    0x25A0: 'coming_soon', 0x25A2: 'apple_logo_44', 0x25A3: 'apple_logo_84_gray', 0x25A4: 'apple_logo_30',
    0x25A5: 'apple_logo_36', 0x25A7: 'status_lock', 0x25A8: 'status_repeat', 0x25A9: 'status_repeat_one',
    0x25AA: 'status_radio', 0x25AB: 'status_eq', 0x25AC: 'status_shuffle', 0x25AD: 'menu_arrow_alt',
    0x25AE: 'menu_arrow', 0x25AF: 'dotted_grid', 0x25B0: 'rating_star', 0x25B1: 'rating_dot',
    0x25B2: 'pixo_logo', 0x25B3: 'music_note_badge', 0x25B4: 'volume_low', 0x25B5: 'volume_high',
    0x25B6: 'contrast_low', 0x25B7: 'contrast_high', 0x25B9: 'clock_colon',
    **{0x25BA + i: f'clock_digit_{i}' for i in range(10)},
    0x25C5: 'battery_charging_anim_5', 0x25C6: 'battery_charging_anim_10', 0x25C7: 'battery_charging',
    0x25C8: 'battery_charged', **{0x25C9 + i: f'battery_level_{i:02d}' for i in range(16)},
    0x25DA: 'big_battery_charging_anim_5', 0x25DB: 'big_battery_charging_anim_10', 0x25DC: 'big_battery_full',
    0x25DD: 'small_clock', 0x25DE: 'scrub_diamond', 0x25DF: 'spin_up_arrow', 0x25E0: 'spin_down_arrow',
    0x25E1: 'tri_up', 0x25E2: 'tri_down', 0x25E3: 'alarm_bell', 0x332F: 'status_pause', 0x3330: 'status_play',
    0x3331: 'status_record', 0x3332: 'status_play_pause_alt', 0x3333: 'disk_mode_no', 0x3334: 'disk_mode_ok',
    0x3335: 'sync_arrows_anim', 0x3337: 'speaker_small_3', 0x3338: 'speaker_small_1',
    0x3339: 'battery_small_1', 0x333A: 'battery_small_2', 0x333B: 'battery_small_3', 0x333C: 'battery_small_full',
    0x333D: 'battery_small_empty', 0x3692: 'dot_4', 0x36AC: 'alarm_big', 0x3C10: 'suit_diamond',
    0x3C11: 'suit_heart', 0x3C12: 'suit_club', 0x3C13: 'suit_spade', 0x3C14: 'hand_open', 0x3C15: 'hand_point',
    0x4483: 'parachute_turret', 0x4484: 'parachute_heli_1', 0x4485: 'parachute_heli_2',
    0x4487: 'parachute_heli_3', 0x4488: 'parachute_heli_4', 0x448A: 'parachute_man_1',
    0x448B: 'parachute_man_2', 0x448D: 'parachute_man_3', 0x4588: 'world_map',
}


def wjson(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)


def wtext(path, s):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(s)


def hexid(i):
    return f'0x{i & 0xffff:04x}' if -0x8000 <= i < 0x10000 else f'0x{i & 0xffffffff:08x}'


def main(ipsw, out, png=True):
    if png:
        from PIL import Image
    fw_name, fw, manifest = firmware.read_ipsw(ipsw)
    ents = firmware.directory(fw)
    osos = next(e for e in ents if e['id'] == 'osos')['data']
    os.makedirs(f'{out}/firmware', exist_ok=True)
    open(f'{out}/firmware/osos.bin', 'wb').write(osos)
    open(f'{out}/firmware/osos_base0.elf', 'wb').write(firmware.wrap_elf(osos, 0))
    wjson(f'{out}/firmware/directory.json', dict(file=fw_name, entries=[{k: v for k, v in e.items() if k != 'data'} for e in ents]))

    rf = rsrc.ResourceFile(osos, rsrc.MAIN_RESOURCE_FILE)
    strings = {rid: rf.string(rid) for rid in rf.ids('Str ')}

    # ---- strings (English + every language file) -----------------------------------
    wjson(f'{out}/data/strings/en.json', {hexid(k): v for k, v in strings.items()})
    variants = {}
    for lf in rsrc.language_files(osos):
        tbl = {rid: lf.string(rid) for rid in lf.ids('Str ')}
        lang = LANG_BY_SETTINGS.get(tbl.get(-89, ''))
        if tbl.get(0x2602) == 'バックライト':
            lang = 'ja'
        elif tbl.get(0x2602) == '背光燈':
            lang = 'zh-Hant'
        if lang is None:
            variants[tbl.get(0x81, '?')] = variants.get(tbl.get(0x81, '?'), 0) + 1
            lang = f"datefmt-{tbl.get(0x81, 'x').lower()}-{variants[tbl.get(0x81, '?')]}"
        wjson(f'{out}/data/strings/{lang}.json', {hexid(k): v for k, v in tbl.items()})

    # ---- types (text styles) -----------------------------------------------------------
    types = {rid: rsrc.parse_type(rf.get('Type', rid), strings) for rid in rf.ids('Type')}
    wjson(f'{out}/data/types.json', {hexid(k): v for k, v in types.items()})

    # ---- menus -----------------------------------------------------------------------
    menus = {rid: rsrc.parse_menu(rf.get('Menu', rid)) for rid in rf.ids('Menu')}
    mj = {}
    for rid, items in menus.items():
        mj[hexid(rid)] = [dict(label=strings.get(it['label']), label_id=hexid(it['label']),
                               submenu=hexid(it['submenu']) if it['submenu'] else None, command=hex(it['command']),
                               accessory=(('bitmap ' + hexid(it['accessory'])) if it['accessory_kind'] == 8 else
                                          None if it['accessory_kind'] == 0x5001 else
                                          f"value {hexid(it['accessory'])} from class {it['accessory_kind']:#x}"),
                               flags=hex(it['flags']), item_class=hex(it['item_class'])) for it in items]
    wjson(f'{out}/data/menus.json', mj)

    # ---- bitmaps -----------------------------------------------------------------------
    bitmaps, bj = {}, {}
    for rid in rf.ids('BMap'):
        bm = rsrc.parse_bmap(rf.get('BMap', rid))
        bitmaps[rid] = bm
        name = f"{rid:04x}_{BITMAP_NAMES.get(rid, 'unnamed')}"
        if png:
            im = Image.new('L', (bm['w'], bm['h']))
            im.putdata([(255, 170, 85, 0)[v] for r in bm['px'] for v in r])
            for scale in (1, 2):
                d = f'{out}/assets/bitmaps/{scale}x'
                os.makedirs(d, exist_ok=True)
                im.resize((bm['w'] * scale, bm['h'] * scale), Image.NEAREST).save(f'{d}/{name}.png')
            alpha = Image.new('LA', (bm['w'], bm['h']))
            alpha.putdata([(0, (0, 85, 170, 255)[v]) for r in bm['px'] for v in r])
            os.makedirs(f'{out}/assets/bitmaps/alpha', exist_ok=True)
            alpha.save(f'{out}/assets/bitmaps/alpha/{name}.png')
        bj[hexid(rid)] = dict(name=BITMAP_NAMES.get(rid), w=bm['w'], h=bm['h'], depth=bm['depth'], file=f'{name}.png')
    wjson(f'{out}/assets/bitmaps/bitmaps.json', bj)
    wtext(f'{out}/lvgl/ipod_images.c', lvgl_images(bitmaps))
    wtext(f'{out}/lvgl/ipod_images_x2.c', lvgl_images(bitmaps, 2))
    wtext(f'{out}/lvgl/ipod_images.h', lvgl_images_h(bitmaps))

    # ---- fonts ---------------------------------------------------------------------------
    fonts = pixofont.all_fonts(osos)
    fj, decls = [], []
    for f in fonts:
        slug = pixofont.slug(f)
        wtext(f'{out}/assets/fonts/bdf/{slug}.bdf', pixofont.to_bdf(f))
        fj.append(dict(name=f['name'], size=f['size'], bold=f['bold'], glyphs=f['nglyphs'],
                       unique_bitmaps=len(f['loc']), fixed_cell=f['fixed_cell'], strike_height=f['strike_h'],
                       baseline_row=f['baseline_row'], line_height=f['line_height'], ascent=f['ascent'],
                       descent=f['descent'], leading=f['leading'], unicode_ranges=_ranges(f['cmap']),
                       charmap_addr=hex(f['charmap_addr']), data_addr=hex(f['data_addr']), bdf=f'bdf/{slug}.bdf'))
        if f['name'] in LATIN_FONTS:
            for scale in (1, 2):
                src, sym = pixofont.to_lvgl_c(f, scale)
                wtext(f'{out}/lvgl/fonts/{sym}.c', src)
                decls.append(sym)
    wjson(f'{out}/assets/fonts/fonts.json', fj)
    wtext(f'{out}/lvgl/fonts/ipod_fonts.h', '#pragma once\n#include "lvgl.h"\n\n' +
          ''.join(f'LV_FONT_DECLARE({s});\n' for s in decls))
    if png:
        font_specimen(fonts, f'{out}/assets/fonts/specimen.png')

    # ---- views -----------------------------------------------------------------------------
    allv = {rid: views.parse_view(rf.get('View', rid)) for rid in rf.ids('View')}
    vj = {}
    for rid, els in allv.items():
        vj[hexid(rid)] = [dict(cls=hex(e['cls']), cls_name=e['cls_name'], ref=f"{e['ref_type']} {hexid(e['ref_id']) if isinstance(e['ref_id'], int) else e['ref_id']}",
                               owner=hex(e['owner'] & 0xffffffff), flags=hex(e['flags']),
                               top=views.describe_constraint(0, e['cons'][0]), left=views.describe_constraint(1, e['cons'][1]),
                               bottom=views.describe_constraint(2, e['cons'][2]), right=views.describe_constraint(3, e['cons'][3]),
                               text_type=(hexid((e['extra'][0] >> 16) & 0xffff) + ' ' + _type_name(types, (e['extra'][0] >> 16) & 0xffff))
                               if e['ref_type'] in ('Str ', 'StSt') and e['extra'] else None,
                               static_string=strings.get(sid16(e['extra'][0])) if e['ref_type'] in ('Str ', 'StSt') and e['extra'] else None,
                               extra=e['extra']) for e in els]
    wjson(f'{out}/data/views.json', vj)

    # ---- mockups -----------------------------------------------------------------------------
    if png:
        R = views.Renderer(allv, bitmaps, types, fonts, strings, menus)
        for name, ctx in mockup_scenes(strings).items():
            cv = views.Canvas()
            ctx = dict(ctx)
            R.render_view(cv, 0x2502, views.Rect(0, 0, 128, 160), ctx)
            for scale in (1, 2):
                cv.to_image(scale).save(f'{_mk(out + "/mockups")}/{name}@{scale}x.png')
    print('wrote', out)


def _mk(d):
    os.makedirs(d, exist_ok=True)
    return d


def _type_name(types, tid):
    t = types.get(tid)
    return f"{t['family']} {t['size']}{' bold' if t['bold'] else ''} {t['align']}" if t else '?'


def _ranges(cmap):
    cps = sorted(cmap)
    out, i = [], 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1:
            j += 1
        out.append(f'U+{cps[i]:04X}-U+{cps[j]:04X}')
        i = j + 1
    return out


def font_specimen(fonts, path):
    from PIL import Image
    sample = 'Now Playing  Music  Settings  Shuffle Songs  1 of 12  0:42 -3:17  ÄÖÜ éñ ΑΩ ЖЯ …'
    rows = []
    for f in fonts:
        if f['name'] not in LATIN_FONTS:
            continue
        label = f"{f['name']} {f['size']}{' bold' if f['bold'] else ''}: "
        w = pixofont.text_width(f, label + sample) + 4
        cv = views.Canvas(w, f['strike_h'] + 4)
        cv.text(f, label + sample, views.Rect(2, 2, 2 + f['strike_h'], w), 'left', ellipsis=False)
        rows.append(cv.to_image(2))
    W = max(r.width for r in rows)
    sheet = Image.new('RGB', (W, sum(r.height for r in rows)), (255, 255, 255))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.height
    sheet.save(path)


def mockup_scenes(strings):
    title_bar = {0x261D: True, 0x261E: True, 0x3335: False, 0x2621: False, -182: False, 0x334A: True}
    play = {0x332E: 0x3330}
    main_items = [dict(label=strings[0x25F3], accessory=0x25AE), dict(label=strings[0x2604], accessory=0x25AE),
                  dict(label=strings[-89], accessory=0x25AE), dict(label=strings[0x260B]), dict(label=strings[0x2602])]
    music = [dict(label=strings[x], accessory=0x25AE) for x in (-90, -91, -92, -93, -94, -95, -96, -97)]
    settings_vals = ['', '', 'Off', 'Off', '10 Seconds', 'Normal', 'Off', 'Off', '', 'Speaker', '', '', 'English', '', '']
    settings = []
    for i, x in enumerate((-105, -106, -107, -108, -109, -34, 0x27BE, -121, -122, -123, -124, -125, -40, -126, -127)):
        settings.append(dict(label=strings.get(x, '?'), value=settings_vals[i] or None,
                             accessory=0x25AE if not settings_vals[i] else None))
    base = dict(show=title_bar, bmap=play)
    return {
        'main_menu': dict(base, text={0x261F: 'iPod'}, deck={0x250D: 0x25E4}, items=main_items,
                          list_count=len(main_items), list_visible=6, selected=0, scroll_needed=False),
        'music_menu': dict(base, text={0x261F: strings[0x25F3]}, deck={0x250D: 0x25EA}, items=music,
                           list_count=len(music), list_visible=6, selected=0, scroll_needed=True),
        'music_menu_scrolled': dict(base, text={0x261F: strings[0x25F3]}, deck={0x250D: 0x25EA}, items=music,
                                    list_count=len(music), list_top=2, list_visible=6, selected=7, scroll_needed=True),
        'settings': dict(base, text={0x261F: strings[-89]}, deck={0x250D: 0x278E}, items=settings,
                         list_count=len(settings), list_visible=6, selected=2, scroll_needed=True),
        'now_playing': dict(base, text={0x261F: strings[0x25F0], 0x3326: '3 of 12', 0x3320: 'Hey Ya!',
                                        0x3321: 'OutKast', 0x3322: 'Speakerboxxx/The Love Below',
                                        0x3323: '1:23', 0x3324: '-2:34', 0x3366: ''},
                            deck={0x250D: 0x2626, -223: 0x2634}, show={**title_bar, 0x2625: False, 0x3366: False},
                            bmap={**play, 0x2624: 0x25AC, 0x2623: 0x25A8}, slider={0x3325: 0.35}),
        'now_playing_volume': dict(base, text={0x261F: strings[0x25F0], 0x3326: '3 of 12', 0x3320: 'Hey Ya!',
                                               0x3321: 'OutKast', 0x3322: 'Speakerboxxx/The Love Below'},
                                   deck={0x250D: 0x2626, -223: 0x2637}, show={**title_bar, 0x2625: False, 0x3366: False},
                                   bmap={**play, 0x2624: 0x25AC, 0x2623: 0x25A8}, slider={0x333E: 0.6}),
    }


# ---- LVGL images: alpha-only A8 (gray level -> coverage), recolour with lv_obj_set_style_image_recolor ----
# (LVGL 9.5's software renderer decodes A8 but not A1/A2/A4.)
def lvgl_images(bitmaps, scale=1):
    sfx = f'_x{scale}' if scale != 1 else ''
    out = ['/*', f' * iPod 4G firmware 3.1.1 BMap resources as LVGL 9 alpha images (LV_COLOR_FORMAT_A8){", pixel-doubled" if scale != 1 else ""}.',
           ' * Alpha = ink coverage (white 0x00, light 0x55, dark 0xAA, black 0xFF). Draw with',
           ' * image_recolor black at opa cover, or white on a highlighted (inverted) row. Art (c) Apple.',
           ' * Generated by re/tools/extract.py.', ' */', '#include "ipod_images.h"', '']
    for rid, bm in bitmaps.items():
        w, h = bm['w'] * scale, bm['h'] * scale
        data = bytearray()
        for row in bm['px']:
            line = bytearray(v * 0x55 for v in row for _ in range(scale))
            data += line * scale
        sym = f"ipod_img_{rid:04x}{sfx}"
        rows = [', '.join(f'0x{v:02x}' for v in data[k:k + 16]) for k in range(0, len(data), 16)]
        out.append(f"/* {BITMAP_NAMES.get(rid, 'unnamed')} {w}x{h} */")
        out.append(f"static const uint8_t {sym}_map[] = {{\n    " + ',\n    '.join(rows) + '\n};')
        out.append(f"const lv_image_dsc_t {sym} = {{\n    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0, "
                   f".w = {w}, .h = {h}, .stride = {w}}},\n    .data_size = sizeof({sym}_map),\n    .data = {sym}_map,\n}};\n")
    return '\n'.join(out)


def lvgl_images_h(bitmaps):
    lines = ['#pragma once', '#include "lvgl.h"', '', '/* <name> is native 160x128 size, <name>_x2 is pixel-doubled. */', '']
    for rid, bm in bitmaps.items():
        n = BITMAP_NAMES.get(rid)
        lines.append(f"extern const lv_image_dsc_t ipod_img_{rid:04x}, ipod_img_{rid:04x}_x2;" + (f"  /* {n} {bm['w']}x{bm['h']} */" if n else ''))
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--no-png']
    main(args[0], args[1], png=len(args) == len(sys.argv) - 1)

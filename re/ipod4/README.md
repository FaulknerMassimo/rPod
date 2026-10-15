# iPod 4G firmware 3.1.1 — UI extraction

Static reverse engineering of `iPod_4.3.1.1.ipsw` to build a near-identical iPod 4G UI for rPod.
Read **[SPEC.md](SPEC.md)** first.

rPod's build only needs `out/lvgl/`: `make ipod-assets` at the top level downloads the IPSW from
Apple into this directory, checks its hash, and runs `tools/extract.py --no-png`, which needs nothing
but python3. For everything below, PNGs included:

```
python3 -m venv .venv && .venv/bin/pip install pillow
.venv/bin/python -I tools/extract.py iPod_4.3.1.1.ipsw out
```

## What `out/` contains

| Path | Contents |
|---|---|
| `firmware/` | `osos.bin` (retail OS image), `osos_base0.elf` (same bytes wrapped as an ARM ELF at base 0, for Hopper/Ghidra/IDA), `directory.json` |
| `assets/bitmaps/{1x,2x,alpha}/` | all 94 `BMap` resources as PNG (opaque gray, pixel-doubled, black+alpha); `bitmaps.json` index |
| `assets/fonts/` | every Pixo font as BDF, `fonts.json` metrics/coverage, `specimen.png` |
| `lvgl/fonts/` | LVGL 9 `lv_font_t` sources for Chicago 12, Espy Sans ×3, Pixo ×13, each at 1× and 2× (`*_x2`); `ipod_fonts.h` |
| `lvgl/ipod_images*.c/.h` | every bitmap as an LVGL 9 A8 image (`ipod_img_<id>`, `ipod_img_<id>_x2`); recolour black/white |
| `web/` | for rPod's web UI: Chicago 12 and Pixo 7 bold as TrueType (pixel outlines, 1/16 em a pixel), every named bitmap as an SVG mask, `manifest.json` |
| `data/strings/*.json` | the 550 English strings plus 13 languages (and 5 regional date-format variants), keyed by resource id |
| `data/menus.json` | 25 menu resources (labels, submenus, accessories, command ids) |
| `data/views.json` | 327 view resources with constraints in readable form |
| `data/types.json` | 120 text styles (font family, size, bold, alignment) |
| `mockups/` | screens rendered from the view data by `tools/ipod4re/views.py`, at 1× and 2× |

## Tools

- `tools/ipod4re/firmware.py`: IPSW and Apple firmware container, ELF wrapper
- `tools/ipod4re/rsrc.py`: Pixo resource files (BMap/Str/Menu/Type)
- `tools/ipod4re/pixofont.py`: Pixo fonts, BDF and LVGL export
- `tools/ipod4re/webfont.py`: TrueType and SVG export for the web UI
- `tools/ipod4re/views.py`: View decoder, constraint layout, reference renderer
- `tools/lvgl_check/`: headless LVGL 9.5 build that renders the generated fonts and images. It was
  checked pixel-identical against `views.Canvas`.

The art, fonts and strings in `out/` are Apple copyright, so don't commit them to a public repo.

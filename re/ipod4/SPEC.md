# iPod 4G (click wheel) UI — reverse-engineering spec for rPod

Source: `iPod_4.3.1.1.ipsw` (FamilyID 4 = 4th-gen grayscale click-wheel iPod, firmware **3.1.1**,
BuildID 0x03118000). Everything here was recovered statically from the shipped `osos` image. I used
REA with the Hopper provider for the artifact inventory, extraction, xrefs and decompilation, plus
capstone disassembly and format parsers in `tools/`. Nothing was executed on hardware or in an
emulator.

Confidence tags used below:

- **[data]**: read directly from a decoded resource, such as a View constraint, a Menu item or a bitmap.
- **[code]**: read from disassembly, with the address given.
- **[inferred]**: consistent with the data and code but not traced end to end.
- **[known]**: from familiarity with the real device, not recovered from this firmware.

Everything regenerates with `python3 -I tools/extract.py iPod_4.3.1.1.ipsw out` (needs Pillow).

---

## 1. Firmware layout

| Item | Where | Notes |
|---|---|---|
| Container | `Firmware-4.3.1.1` in the IPSW zip | "STOP" banner, `]ih[` header @0x100, directory @0x4200 (40-byte entries) |
| `osos` (retail OS) | devOffset 0x4400, 3,501,844 B, checksum OK | ARM7TDMI (PP5020). Linked at **virtual 0**: reset code programs the PP5020 memory mapper (`0xF000F000`) to alias SDRAM `0x10000000` at 0, then jumps to `0x228` [code @0x1EC] |
| `aupd` | devOffset 0x35B600 | flash updater, encrypted/compressed; not needed |
| BSS / heap | `0x1039_0000`+ (physical alias) | startup panics if image end (0x356F10) overlaps it [code @0x268] |
| Vtables | `0x20000–0x29FFF` | entries are **offsets relative to the vtable** (ROPI code), so no absolute code pointers |
| Main resource file | `0x1A5078` | BMap 94, Menu 25, Str 550, View 327, Type 120, TCmd 1, LDTm 1 |
| Language resource files | `0x175788 … 0x1A1758` | Str (+Type for CJK) per language |
| Font tables | `0x331518` (13 Pixo), `0x331A08` (43 others) | (charmap, fontdata) pointer pairs |

The UI toolkit is **Pixo**. Pixo wrote the original iPod OS, and its logo is resource `0x25B2`.

## 2. Display and colour model

- **Screen:** 160 × 128, 2 bpp grayscale. The LCD is driven via the PP5020 LCD bridge at `0x70003000` [code @0x147C08].
- **Palette:** the toolkit uses RGB colours that the LCD quantises to 4 grays. They're initialised at `0x1C8FC` [code]:

  | Global | RGB | 2-bpp level |
  |---|---|---|
  | `0x1041A240` | #FFFFFF white | 0 |
  | `0x1041A244` | #AAAAAA light gray | 1 |
  | `0x1041A248` | #7F7F7F mid gray | (1/2) |
  | `0x1041A24C` | #555555 dark gray | 2 |
  | `0x1041A250` | #000000 black | 3 |

- **Bitmaps:** 1 or 2 bpp, MSB-first. 0 is white and 3 is black, and icons use levels 1–2 as anti-aliasing.
- **Selection:** the UI is black-on-white, and the selected row is inverted to white-on-black. ImageView's Draw picks draw mode 3 vs 1 from view-flag bit 3 [code @0x5A8C8]. That's why the `>` arrow exists only once, as a black-on-white 1-bpp bitmap; the selected state inverts it [inferred].

## 3. The Pixo view system (how every screen is built)

Screens are **declarative `View` resources**: trees of elements laid out with edge constraints. The
code only fills in dynamic text and images through data binding. The format is fully decoded in
`tools/ipod4re/views.py`, and every view is in `out/data/views.json` with readable constraints.

### 3.1 Element record

```
u32 size | u32 class | fourcc refType ('None','Str ','BMap','Cntl','StSt') | u32 refId
s32 owner (class that resolves refId: 0x2B00 system, 0x3300 Now Playing, 0x2780 Settings, 0x4580 Clock, 8/-1 global)
u32 aux | u32 flags (0x800 = visible by default, 0x8000 on a plain View = solid black fill)
4 × constraint {ref, kind, value}   slots: 0 top, 1 left, 2 bottom, 3 right
s32 extra[] — Text: (TypeId << 16 | staticStringId); AnimatedImage: (frameMs, frameH, loop)
```

### 3.2 Constraint semantics [data + code @0x34824 / 0xCE384]

| kind | meaning |
|---|---|
| `0x8000_0001` | absolute offset from parent origin |
| `0x8000_0002` | offset from the parent's **same** edge (right/bottom offsets are negative inward) |
| `0x8000_0004` / `0x8000_0008` | width / height = value |
| `+0x10` (`…14`, `…18`) | auto-size to content (text width, bitmap size); value = default/preferred |
| `0x8000_0022` | centre in parent (size from the opposite slot) |
| `0x100/0x200/0x300/0x400` (no high bit) | sibling's top/left/bottom/right + value; ref 0 = previous sibling, else sibling id |
| ref 1 + kind 1/2 | offset from parent centre line |

Rects are Mac-style `{top, left, bottom, right}` with exclusive right/bottom. `DrawLine(port, x0, y0, x1, y1)`
has inclusive endpoints [code @0x5AB30].

### 3.3 Classes (is-kind-of chains from the vtables) [code]

`View 0x1080` → `Group 0x1100` → {`ScrollList 0x1180`, `Window 0x1200`, `Deck 0x2F00` (one visible card), `Battery 0x4980`, `MenuBase 0x1700` → `Menu 0x1800`, `MenuRow 0x1780`};
`View` → `Image 0x1280` → `AnimatedImage 0x1300`; `View` → `Text 0x1580` → `MarqueeText 0x3180`;
`View` → `Control 0x1400` → {`Slider 0x3080` (progress/volume), `0x4800` rating, `0x1980` → `ScrollBar 0x1F00`}.
Vtable slot 25 = Draw, slot 9 = HandleEvent (events are FourCCs).

### 3.4 Data binding [code @0x75584, @0x7492C]

`GetResource(owner, type, id)` walks up the view's parent chain and calls each object's FindResource
(vtable slot 20). App modules answer dynamically. For example, the Now Playing module's switch at
`0x7492C` supplies:

- `Str 0x3326` = "%d of %d" (string 0x3328)
- `0x3320–0x3322` = the three centred lines (title, artist, album) [inferred from layout]
- `0x3323` / `0x3324` = elapsed / remaining time
- `BMap 0x332E` = play/pause icon
- `VMin`/`VMax`/value for the sliders

**Implication for rPod:** keep the same split. Static layout comes from the view spec in section 5, and live values come from MPD.

## 4. Fonts

Pixo bitmap fonts: a 1-bpp strike plus a location table `{bitStart, bitEnd, advance, s16 xOffset}`, and
a Unicode range map (glyph g uses `loc[index[g-1]]`). All are exported as BDF, specimen PNG and
**LVGL 9 C fonts at 1× and 2×**. The LVGL fonts were verified pixel-identical against the reference
renderer, using a headless LVGL 9.5 build from rPod's vendored tree.

| Font | Strike h | Baseline row | Line h | Glyphs | Used for |
|---|---|---|---|---|---|
| **Chicago 12** | 16 | 12 (declared ascent 13) | 16 | 579 (Latin-1/Ext, Greek, Cyrillic, ™ …) | titles, every menu row, values, Now Playing text and times [data: Types 0x2576/7/8] |
| **Pixo 7 bold** | 10 | 8 | 11 | 224 | Now Playing "N of M" (Type 0x2554) |
| Pixo 5–14 (5,5b,6,6b,7b,8,8b,9,9b,10b,11b,12b,14b) | 7–19 | = ascent | | 224 | Pixo 6 centre: one label in the Calendar/Contacts group (view 0x3685); others not referenced by any Type |
| **Espy Sans 10 / 10 bold / 9 bold** | 14/13/13 | 11/11/10 | 15/17/17 | 223–489 | Contacts, Calendar, Notes, photo import, alarm, memory/test screens, podcast line in Now Playing |
| AppleGothic 12, AppleLiGothicMedium 12, Hei 12, Osaka 12/10 | 12–16 | | | CJK, split by Unicode block | Korean / Traditional Chinese / Simplified Chinese / Japanese UI and tags (fixed-cell sub-fonts) |

Text Types also name Charcoal, Geneva, Techno and Verdana, but this build ships no glyph data for them
(they fall back).

**Vertical placement** [inferred]: a text element centres the font strike in its rect. So in an
18-px row, the Chicago strike top sits at row + 1, the cap tops at row + 4 and the baseline at row + 13.

## 5. Screen layouts (logical 160×128 pixels)

All numbers are [data] from the View resources unless marked. See `out/mockups/*.png` (rendered by `tools/ipod4re/views.py`).

### 5.1 Root and title bar (View 0x2501 → 0x2502 → 0x2503)

- Title bar: y = 0…18 (19 px). Content area: y = 19…127 (109 px), a Deck holding every app screen (View 0x250D).
- **Rule:** 1-px black line at y = 18, full width (plain View, flags 0x8800, height 1, bottom-anchored).
- **Play state icon** (slot `0x332E` → `0x3330` play / `0x332F` pause): 20×10 bitmap at (4, 4).
- **Hold/lock icon** (`0x25A7`, 8×10): x = icon.right + 0 = 24, y = 3. Shown while the hold switch is on.
- **Battery:** 25×10 at x = 134…158 (right edge 1 px from screen edge), y = 4.
  - Levels `0x25C9…0x25D8` (16 steps, empty → full); `0x25C8` = charged; `0x25C7` = charging.
  - Charging animation: `0x25C5`/`0x25C6` frame strips (5/10 frames of 10 px), 500 ms per frame.
- **Title text:** Chicago 12 centred in x = 37…122, full bar height, truncated with "…".
- **Sync** (animated `0x3335`, 4 frames of 14×14 at 500 ms): at (4, 2) instead of the play icon.
- **"Time in Title"** variant (View 0x2883): clock text, Chicago 12 right-aligned, ending 5 px left of the battery. The left-aligned title starts at lock.right + 5.

### 5.2 Menu / list screens (e.g. main menu card 0x25E4, Music 0x25EA)

- **ScrollBar** (class 0x1F00): right-anchored, y = 19…127. Its width is **10 px when the list content is
  taller than the list, else 0** [code @0x3EF98: width = preferred width captured at first layout (@0x65F34), or 0].
- **List:** x = 0 … scrollbar.left, y = **20** … 127 (top = scrollbar.top + 1, leaving a 1-px white gap under
  the title rule). So **6 rows × 18 px exactly** (20 + 108 = 128).
- **Row template** (View −184, main menu style):
  - Row: 18 px tall, full list width.
  - Label: Chicago 12 left-aligned, **x = 6 … rowRight − 14**, full row height, "…" truncation.
  - Accessory bitmap (the `>` is `0x25AE`, 12×9): right edge at **rowRight − 2**, top at row + 5 (row centre − 4).
- **Settings row template** (View −255, used by Settings and Main Menu config):
  - Accessory: same as above (0 width when the item has none).
  - Value: Chicago 12 right-aligned, auto width, right edge = accessory.left.
  - Label: x = 6 … value.left − 6.
- **Selected row:** the full row rect (x = 0 … rowRight, 18 px) is inverted: black fill, white text and icons [inferred + known].
- **Scrollbar drawing** [code @0x5A6E0 + 0x3C49C], scrollbar rect x = L…L+9:
  - Background white.
  - 1-px black rules at x = L+1 and x = L+9, the full height, plus a bottom rule at y = 127. **No top rule** (the title rule sits above).
  - Thumb: black, x = L+3 … L+7 (5 px wide). Its track is y = 20 … 125, leaving a 1-px gap top and bottom.
  - Thumb height = max(3, visible × track / content), with track = 106 px.
  - Thumb top = 20 + scrollPx × (track − thumb) / (contentPx − visiblePx).
  - If view-flag bit 25 is clear, the thumb is a fixed 3 px.

### 5.3 Now Playing (card 0x2626; absolute y)

- **Header (y = 23):**
  - "N of M": Pixo 7 bold at x = 5, height 9, auto width.
  - A dynamic 9×9 icon (slot 0x2625) at x = 83, followed by Pixo 7 bold text (0x3366) at x = 93. Hidden in the mockup; purpose not identified.
  - Repeat icon slot (0x2623, `0x25A8`/`0x25A9`) at x = 127…140; shuffle slot (0x2624, `0x25AC`) at x = 142…155.
- **Track text (y = 39…86):** three lines, Chicago 12 centred, x = 2…157, each 15 px tall with 1-px gaps, at **y = 39, 55, 71** (title, artist, album).
  - The title line is `MarqueeText`, extra params (140, 5000, 4, 25). It scrolls when too long; my guess is 5 s before scrolling, but the field meanings aren't decoded.
- **Bottom deck (y = 87…127, 41 px)** shows one card at a time:
  - **Progress** (0x2634): bar **x = 8…151, y = 102…110** (144 × 9). Rounded black frame [code @0x5AB30]: edges inset 2 px at the corners, plus corner pixels at (1,1), (w−2,1), (1,h−2), (w−2,h−2). The elapsed part is filled **#555555** inside a 1-px inset. Times: Chicago 12, y = 112…126; elapsed left-aligned at x = 8, remaining (e.g. "-2:34") right-aligned ending at x = 151.
  - **Volume** (0x2637): speaker-low `0x25B4` (10×13) at (6, 100); bar x = 20…135, y = 102…110 (same rounded style); speaker-high `0x25B5` (14×13) at (140, 100).
  - **Rating** (0x263A): control 105 × 23 at x = 28 (centre − 52), y = 95. Five slots on a **21-px pitch**: star `0x25B0` for set, dot `0x25B1` for unset [code @0x5AA54].
  - **Scrub / podcast** (0x2639): Espy Sans 10 line at y = 87, then the bar with the diamond knob `0x25DE`.
  - **Audiobook speed** (0x263B): "Speed" label with the value right-aligned.

### 5.4 Other screens (Deck cards in View 0x250D)

| Card | Screen | Key content |
|---|---|---|
| 0x25E4 | Main menu | Menu 0x2512 |
| 0x25EA | Music | Menu 0x25EE |
| 0x260D–0x2616, 0x3309 | Browse lists (Artists/Albums/Songs/…), On-The-Go | list rows; MarqueeText for long names; `0x3368` speaker glyph marks the playing track |
| 0x25E7 / 0x25E8 / 0x25E9 | Extras / Games / game menu | Menus 0x25EB / 0x25EC / 0x25ED |
| 0x278E / 0x2790 | Settings / Settings ▸ Main Menu | Menus 0x278D / 0x278F (value rows) |
| 0x2792, 0x2788, 0x2797, 0x279B, 0x2862, 0x2796, 0x2794 | settings option lists, time zones, sort/display, date & time | |
| 0x27B4 | Contrast | Slider between `0x25B6`/`0x25B7` |
| 0x285B / 0x4594 | Set date & time / alarm time | spinner digits with `0x25DF`/`0x25E0` arrows |
| 0x2630 | About | Songs / Capacity / Available / Version / S/N / Model, values right-aligned |
| 0x2629 | Legal | `0x25A5` logo + © text |
| 0x2627 | Disk mode | `0x3333` "no" / `0x3334` check (64×64) + "Do not disconnect." / "OK to disconnect." |
| 0x2628 | Charging | big battery animation `0x25DA`/`0x25DB`, full `0x25DC` (92×40 frames) |
| 0x2617 | full-screen centred message | Chicago 12 centre |
| 0x261A | Cancel / Confirm sheet | Menu 0x25EF |
| 0x360D–0x368E, 0x4180–0x419A | Contacts / Calendar / Notes | Espy Sans |
| 0x3D04–0x3D1B, 0x4293–0x42B2 | Voice memos, photo import | |

## 6. Menu tree [data: Menu resources; strings in `out/data/strings/en.json`]

- **Main Menu** (0x2512): Music ▸, Playlists ▸, Artists ▸, Albums ▸, Songs ▸, Podcasts ▸, Genres ▸, Composers ▸,
  Audiobooks ▸, Extras ▸, Clock ▸, Contacts ▸, Calendar ▸, Calendars ▸, Notes ▸, Photo Import ▸, Voice Memos ▸,
  Games ▸, Settings ▸, Shuffle Songs, Backlight, Sleep, Now Playing ▸.
  - Visibility is user-configurable (Settings ▸ Main Menu).
  - Factory default [known; the defaults table hasn't been decoded]: **Music, Extras, Settings, Shuffle Songs, Backlight**, with **Now Playing** appended while something is loaded.
- **Music** (0x25EE): Playlists, Artists, Albums, Songs, Podcasts, Genres, Composers, Audiobooks.
- **Extras** (0x25EB): Clock, Contacts, Calendar, Calendars, Notes, Photo Import, Voice Memos, Games, Testing.
- **Games** (0x25EC): Brick, Music Quiz, Parachute, Solitaire.
- **Settings** (0x278D): About ▸, Main Menu ▸, Shuffle [value], Repeat [value], Backlight Timer [value], Audiobooks [speed],
  EQ [value], Sound Check [value], Contrast ▸, Clicker [value], Date & Time ▸, Contacts ▸, Language ▸, Legal ▸, Reset All Settings ▸.
- **Backlight Timer** (−110): Off, 2 Seconds, 5 Seconds, 10 Seconds, 20 Seconds, Always On.
- **Sleep Timer** (−171): Off, 15/30/60/90/120 Minutes.
- **Audiobooks speed** (−117): Slower, Normal, Faster.
- **Clicker strings:** Speaker / Headphones / Off. **EQ presets:** Acoustic … Vocal Booster (see strings).
- **Language** (0x2799): 14 languages. **Time zones** (0x2863): 50 entries.
- **Item flags:** 0x20 base, +0x02 has accessory, +0x04 has a numeric shortcut key (a Pixo phone-toolkit
  leftover, the first nine items only). An accessory is either a bitmap (kind 8, the `>` arrow) or a value
  string supplied by the owning app (kind = class id, e.g. 0x2780).

## 7. Behaviour notes

- **Events** are FourCCs [code]:
  - Keys: `kDwn`, `kUp `, `kAut` (auto-repeat), `kHld`, `kP&H` (press and hold).
  - Wheel and scrolling: `Weel`, `ScUp`/`ScDn`, `vScl`.
  - Hold switch: `LOCK`/`!lck`.
  - Also `MnuS` (menu select), `vRsz`, `PChg`.
  - `Weel` events are built by the constructor at `0xF9AA8`, from the single producer `0xE515C` (posted via `0xAB854`).
- **App key/wheel hook** (function around `0xAC180`): every key or wheel event resets the idle/backlight timers, with constants 1000/80000 ms and a 3000 ms path [code; exact semantics unresolved].
- **Lists** let app modules override scrolling per list through `ScUp`/`ScDn` resources [code @0xC43D0].
- **Not recovered yet:** the click-wheel acceleration curve, menu transition animations (if any), and the backlight fade. See section 9.

## 8. Mapping onto rPod (320 × 240 ST7789V, LVGL 9.5)

Native 2× is 320 × **256**, 16 px (8 logical px) taller than the panel. The recommended approach is
**2× everywhere, with the list row pitch tightened from 36 to 33 px**:

| Element | iPod (logical) | rPod @2× | Notes |
|---|---|---|---|
| Title bar | 19 | 38 (rule at y = 36–37) | icons at 2× positions: play (8,8), battery (268,8) 50×20 |
| Gap under rule | 1 | 2 | |
| List rows | 6 × 18 | **6 × 33** = 198 | Chicago @2× strike is 32 px, so the text sits at row + 0/1 |
| Total | 128 | 238 (+2 spare) | |
| Label inset | x 6, right −14 | x 12, right −28 | |
| Arrow | right −2, centre −4 | right −4, centre −8 (24×18) | |
| Scrollbar | 10 px | 20 px | rules at x+2–3 and x+18–19, thumb x+6…x+15 |

For Now Playing at 2× (content 218 px vs 202 available), remove 8 logical px: move the track-text
block up by 4 (y 39 → 35) and put the progress bar 11 px (not 15) below the deck top. Everything else
scales cleanly.

LVGL integration (generated in `out/lvgl/`):

- **Fonts:** `#include "ipod_fonts.h"`, then use `ipod_font_chicago_12_x2` for menus and titles, and
  `ipod_font_pixo_7_bold_x2` for "N of M". Each font's line height keeps strike row 0 at the label top, so
  `lv_obj_set_y(label, rowTop + (rowH − 32) / 2)` reproduces the original placement.
- **Images:** `ipod_images.h` declares `ipod_img_<id>` (1×) and `ipod_img_<id>_x2` in A8. Draw them with
  `image_recolor` = black (`LV_OPA_COVER`), and switch to white on the selected row.
  - A2 isn't used because LVGL 9.5's software renderer only decodes A8.
- **Colours:** use the palette in section 2. For a period LCD look, tint white toward a pale gray-green; that's a design choice, not recovered.

## 9. Open items / next steps

1. **Wheel acceleration and step size:** trace from the `Weel` producer `0xE515C` into the click-wheel task (`OptoTask`) and the list scroll handler. Not yet recovered.
2. **Marquee timing:** decode MarqueeText (class 0x3180) params `[140, 5000, 4, 25]` / `[140, 1250, 8, 25]`.
3. **Default main-menu visibility table** and default settings values (Settings module class 0x2780, FindResource `0x732D0`).
4. **Selected-row rendering:** confirm the invert path in Text/Row drawing (TextView Draw `0x5AD88`).
5. **Transitions:** check the Deck (0x2F00) HandleEvent `0x812B4` / slot 24 `0x5CC9C` for slide animations.
6. **Pixel-exact ground truth:** booting this osos in a PP5020 emulator (e.g. the open-source *clicky*) would give real screenshots to diff the mockups against.

## 10. Evidence index

- REA `inspect_artifact` `ev_77dbc682…` (IPSW graph); `extract_artifact` `ev_ee9efe54…`; `binary_overview` `ev_42e70c4a…`
  (osos at base 0, 10,326 Hopper procedures); reset/remap assembly `ev_6c42a346…`; task-name xrefs `ev_ff449a9a…`.
- Key code addresses (osos virtual):

  | Address | Function |
  |---|---|
  | `0x1C8FC` | colours |
  | `0x34824` / `0x34868` | auto-size tests |
  | `0xCE384` / `0xCE458` | SetHeight / SetWidth |
  | `0x5A874` | Image Draw |
  | `0x5AD88` | Text Draw |
  | `0x5AA24` → `0x5A6E0` | ScrollBar Draw |
  | `0x3C49C` | thumb rect |
  | `0x3EF98` | scrollbar show/hide |
  | `0x5AB30` | Slider Draw |
  | `0x5AA54` | rating stars |
  | `0x75584` | GetResource |
  | `0x7492C` | Now Playing bindings |
  | `0x82FA0` / `0xCB3B4` | Menu load / set |
  | `0xF9AA8` | wheel event |

- Data: resource file `0x1A5078`, font tables `0x331518` / `0x331A08`, pattern table `0x174C90`.

## 11. Legal

All bitmaps, fonts (Chicago, Espy Sans, Pixo, and the CJK faces) and strings in `out/` are Apple
copyright, extracted from firmware you own for interoperability work. Keep them out of a public repo
(rPod is CC BY-NC); generate them locally from the IPSW with `tools/extract.py` instead of committing
them.

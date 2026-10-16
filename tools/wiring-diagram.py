#!/usr/bin/env python3
"""Generates docs/wiring-zero2w.svg: the Pi Zero 2 W wired to the 2" display
and the click wheel, as docs/wiring.md shows it.

The pin choices are the DISPLAY and WHEEL tables below; the GPIOs come from
docs/PLAN.md §1.2 and system/config.txt.d/rpod.txt, so change those first.
The board is drawn to scale from its mechanical drawing (65 x 30 mm, the
header centred on the long edge, pin 1 at the microSD end); the display and
the wheel aren't. The ribbon's pin numbers are the 4th-gen wheel's, from
https://jasongarr.wordpress.com/project-pages/ipod-clickwheel-hack/.

    python3 -I tools/wiring-diagram.py docs/wiring-zero2w.svg
"""
import sys

W, H = 1700, 1400
FONT = "Helvetica, Arial, 'Liberation Sans', sans-serif"
INK, MUTE, BG = "#1d1d1f", "#5f6368", "#f6f5f1"

# ---- Pi Zero 2 W geometry (mm -> px) -------------------------------------
S = 14.0                      # px per mm
BX, BY = 395.0, 900.0         # board top-left
BW, BH = 65 * S, 30 * S
PITCH = 2.54 * S
COL1_X = BX + 8.37 * S        # pin 1 / pin 2 column
ROW_OUT = BY + 2.23 * S       # even pins, board edge
ROW_IN = BY + 4.77 * S        # odd pins, inner row
BASE_TOP = ROW_OUT - PITCH / 2
BASE_BOT = ROW_IN + PITCH / 2
HOUSING = 25.0


def m(x, y):
    return BX + x * S, BY + y * S


def pin_xy(n):
    col = (n + 1) // 2
    return COL1_X + (col - 1) * PITCH, (ROW_IN if n % 2 else ROW_OUT)


# ---- wires ---------------------------------------------------------------
DISPLAY = [  # (module pin, Pi pin, colour, Pi-side label), top to bottom
    ("VCC", 17, "#d7263d", "3V3"),
    ("GND", 20, "#2b2b2b", "GND"),
    ("DIN", 19, "#1f6fd1", "GPIO 10 · MOSI"),
    ("CLK", 23, "#f2b705", "GPIO 11 · SCLK"),
    ("CS", 24, "#f07b1d", "GPIO 8 · CE0"),
    ("DC", 18, "#2a9d4b", "GPIO 24"),
    ("RST", 13, "#8e44ad", "GPIO 27"),
    ("BL", 33, "#9aa3ad", "GPIO 13 · PWM1"),
]
WHEEL = [  # (ribbon pin, Pi pin, colour, Pi-side label), left to right in the bundle
    (1, 1, "#d7263d", "3V3"),
    (2, 16, "#16a3b8", "GPIO 23"),
    (6, 22, "#e0438c", "GPIO 25"),
    (8, 6, "#2b2b2b", "GND"),
]
RIBBON_NAMES = {8: "GND", 6: "DATA", 5: "n/c", 4: "n/c", 2: "CLOCK", 1: "3V3"}

Y_TRANS_TOP = BASE_TOP - 22          # wires leave the header here...
Y_TRANS_BOT = Y_TRANS_TOP - 150      # ...and are bundled by here
R_TURN = 22.0
SLOT = 16.0

# display module
DM_X, DM_Y = 1235.0, 195.0
DM_W, DM_H = 435.0, 310.0
DCONN_X = DM_X + 18                  # pin centres on its connector
DPIN_Y0 = DM_Y + 50
DPIN_PITCH = 30.0
DSLOT_X0 = 836.0

# click wheel + breakout
WH_CX, WH_CY, WH_R = 172.0, 360.0, 135.0
BO_X, BO_Y, BO_W, BO_H = 336.0, 452.0, 162.0, 270.0
BPIN_X = BO_X + BO_W - 16
BPIN_Y8 = BO_Y + 30                  # pin 8 at the top, pin 1 at the bottom
BPIN_PITCH = 30.0
WSLOT_X0 = 700.0

out = []
add = out.append


def bpin_y(n):
    return BPIN_Y8 + (8 - n) * BPIN_PITCH


def header_exit(n, side):
    """Path from a header pin up to just above the header. Odd (inner-row)
    pins leave through the gap between the outer-row housings, on the side
    the wire is heading."""
    x, y = pin_xy(n)
    if n % 2:
        gx = x + side * PITCH / 2
        return f"M{x:.1f},{y:.1f} L{gx:.1f},{(ROW_OUT + ROW_IN) / 2:.1f} L{gx:.1f},{Y_TRANS_TOP:.1f}", gx
    return f"M{x:.1f},{y:.1f} L{x:.1f},{Y_TRANS_TOP:.1f}", x


def wire(n, slot_x, turn_y, end_x, side, colour):
    d, ex = header_exit(n, side)
    k = (Y_TRANS_TOP - Y_TRANS_BOT) * 0.55
    d += (f" C{ex:.1f},{Y_TRANS_TOP - k:.1f} {slot_x:.1f},{Y_TRANS_BOT + k:.1f}"
          f" {slot_x:.1f},{Y_TRANS_BOT:.1f}")
    d += f" L{slot_x:.1f},{turn_y + R_TURN:.1f}"
    sweep = 1 if side > 0 else 0
    d += f" A{R_TURN},{R_TURN} 0 0 {sweep} {slot_x + side * R_TURN:.1f},{turn_y:.1f}"
    d += f" L{end_x:.1f},{turn_y:.1f}"
    case = "#000" if colour == "#2b2b2b" else "#1d1d1f"
    add(f'<path d="{d}" class="w-case" stroke="{case}"/>')
    add(f'<path d="{d}" class="w-core" stroke="{colour}"/>')
    add(f'<path d="{d}" class="w-shine"/>')


def text(x, y, s, size=15, anchor="start", weight="normal", fill=INK, extra=""):
    add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" text-anchor="{anchor}" '
        f'font-weight="{weight}" fill="{fill}" {extra}>{s}</text>')


# ======================================================================
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
    f'font-family="{FONT}">')
add('<title>rPod wiring: Raspberry Pi Zero 2 W to the 2-inch display and the click wheel</title>')
add("""<style>
  .w-case { fill: none; stroke-width: 9; stroke-linecap: round; stroke-linejoin: round; }
  .w-core { fill: none; stroke-width: 6; stroke-linecap: round; stroke-linejoin: round; }
  .w-shine { fill: none; stroke: #fff; stroke-opacity: .28; stroke-width: 1.6;
             stroke-linecap: round; stroke-linejoin: round; }
</style>""")
add("""<defs>
  <linearGradient id="pcb" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#23824a"/><stop offset="1" stop-color="#17643a"/></linearGradient>
  <linearGradient id="can" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#e4e7ea"/><stop offset=".55" stop-color="#c3c8cd"/>
    <stop offset="1" stop-color="#aab0b6"/></linearGradient>
  <linearGradient id="metal" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#dfe2e5"/><stop offset="1" stop-color="#a9afb5"/></linearGradient>
  <linearGradient id="lcdpcb" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#1f5ea8"/><stop offset="1" stop-color="#174a86"/></linearGradient>
  <radialGradient id="wheel" cx=".45" cy=".4" r=".7">
    <stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#e6e6e3"/></radialGradient>
  <radialGradient id="button" cx=".45" cy=".4" r=".7">
    <stop offset="0" stop-color="#fbfbfa"/><stop offset="1" stop-color="#dcdcd9"/></radialGradient>
  <linearGradient id="flex" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="#d9962a"/><stop offset="1" stop-color="#c47f17"/></linearGradient>
  <filter id="shadow" x="-10%" y="-10%" width="120%" height="125%">
    <feDropShadow dx="0" dy="4" stdDeviation="6" flood-color="#000" flood-opacity=".18"/></filter>
</defs>""")
add(f'<rect width="{W}" height="{H}" rx="18" fill="{BG}"/>')

# ---- title ---------------------------------------------------------------
text(48, 68, "rPod wiring: Raspberry Pi Zero 2 W", 36, weight="bold")
text(48, 104, "Waveshare 2″ ST7789V display and 4th-gen iPod click wheel · "
     "top view, component side up · parts not to scale", 18, fill=MUTE)

# ---- click wheel ---------------------------------------------------------
text(WH_CX, WH_CY - WH_R - 24, "4th-gen iPod click wheel", 20, "middle", "bold")
# ribbon from the wheel to the breakout's FPC socket
fx0, fy0 = WH_CX + 40, WH_CY + WH_R - 12
fx1, fy1 = BO_X - 6, BO_Y + BO_H / 2
rib = f"C{fx0 + 10},{fy0 + 110} {fx1 - 90},{fy1} {fx1},{fy1}"
add(f'<path d="M{fx0},{fy0} {rib}" fill="none" stroke="url(#flex)" stroke-width="30" stroke-opacity=".92"/>')
for i in range(8):
    o = -10.5 + i * 3
    add(f'<path d="M{fx0 + o},{fy0} C{fx0 + 10 + o},{fy0 + 110} {fx1 - 90},{fy1 + o} {fx1},{fy1 + o}" '
        f'fill="none" stroke="#f3c26b" stroke-width="1" stroke-opacity=".8"/>')
add(f'<g filter="url(#shadow)"><circle cx="{WH_CX}" cy="{WH_CY}" r="{WH_R}" fill="url(#wheel)" '
    f'stroke="#c9c9c5" stroke-width="2"/></g>')
add(f'<circle cx="{WH_CX}" cy="{WH_CY}" r="{WH_R * 0.36}" fill="url(#button)" stroke="#c4c4c0" stroke-width="2"/>')
g = "#a2a29e"
text(WH_CX, WH_CY - WH_R * 0.68, "MENU", 17, "middle", "bold", g, 'letter-spacing="1.5"')
nx, ny = WH_CX + WH_R * 0.71, WH_CY            # next
add(f'<path d="M{nx - 15},{ny - 8} l9,8 l-9,8z M{nx - 6},{ny - 8} l9,8 l-9,8z" fill="{g}"/>'
    f'<rect x="{nx + 3}" y="{ny - 8}" width="3" height="16" fill="{g}"/>')
px, py = WH_CX - WH_R * 0.71, WH_CY            # previous
add(f'<path d="M{px + 15},{py - 8} l-9,8 l9,8z M{px + 6},{py - 8} l-9,8 l9,8z" fill="{g}"/>'
    f'<rect x="{px - 6}" y="{py - 8}" width="3" height="16" fill="{g}"/>')
qx, qy = WH_CX, WH_CY + WH_R * 0.71            # play/pause
add(f'<path d="M{qx - 14},{qy - 8} l11,8 l-11,8z" fill="{g}"/>'
    f'<rect x="{qx + 1}" y="{qy - 8}" width="3.5" height="16" fill="{g}"/>'
    f'<rect x="{qx + 8}" y="{qy - 8}" width="3.5" height="16" fill="{g}"/>')

# breakout board: FPC socket on the left, header pins on the right
add(f'<g filter="url(#shadow)"><rect x="{BO_X}" y="{BO_Y}" width="{BO_W}" height="{BO_H}" rx="6" '
    f'fill="#4a2c6e" stroke="#341d50" stroke-width="2"/></g>')
add(f'<rect x="{BO_X - 8}" y="{BO_Y + BO_H / 2 - 40}" width="24" height="80" rx="2" fill="#2a2a2a"/>'
    f'<rect x="{BO_X + 12}" y="{BO_Y + BO_H / 2 - 40}" width="6" height="80" fill="#cdb98f"/>')
text(BO_X + BO_W / 2, BO_Y + BO_H + 28, "8-pin FPC breakout", 17, "middle", "bold")
text(BO_X + BO_W / 2, BO_Y + BO_H + 50, "pins as numbered on the ribbon", 15, "middle", fill=MUTE)
# jumpers: ribbon pins 3 and 7 tied to 1 (3V3)
jx = BPIN_X - 22
jd = (f"M{BPIN_X},{bpin_y(7)} L{jx + 7},{bpin_y(7)} Q{jx},{bpin_y(7)} {jx},{bpin_y(7) + 7} "
      f"L{jx},{bpin_y(1) - 7} Q{jx},{bpin_y(1)} {jx + 7},{bpin_y(1)} L{BPIN_X},{bpin_y(1)} "
      f"M{jx},{bpin_y(3)} L{BPIN_X},{bpin_y(3)}")
add(f'<path d="{jd}" fill="none" stroke="#5a0d16" stroke-width="6" stroke-linejoin="round"/>')
add(f'<path d="{jd}" fill="none" stroke="#d7263d" stroke-width="3.5" stroke-linejoin="round"/>')
for n in range(1, 9):
    y = bpin_y(n)
    add(f'<rect x="{BPIN_X - 5}" y="{y - 5}" width="10" height="10" fill="#d4af37" stroke="#8a6d12"/>')
    text(BO_X + 44, y + 5, str(n), 14, "end", "bold", "#fff")
    if n in RIBBON_NAMES:
        nc = RIBBON_NAMES[n] == "n/c"
        text(BO_X + 52, y + 5, RIBBON_NAMES[n], 14, "start", "normal" if nc else "bold",
             "#b9a3d6" if nc else "#fff")

# ---- display module ------------------------------------------------------
text(DM_X + DM_W / 2, DM_Y - 22, "Waveshare 2″ LCD module (ST7789V, SPI)", 20, "middle", "bold")
add(f'<g filter="url(#shadow)"><rect x="{DM_X}" y="{DM_Y}" width="{DM_W}" height="{DM_H}" rx="10" '
    f'fill="url(#lcdpcb)" stroke="#123a69" stroke-width="2"/></g>')
for hx, hy in ((DM_X + 14, DM_Y + 14), (DM_X + DM_W - 14, DM_Y + 14),
               (DM_X + 14, DM_Y + DM_H - 14), (DM_X + DM_W - 14, DM_Y + DM_H - 14)):
    add(f'<circle cx="{hx}" cy="{hy}" r="8" fill="#d4af37"/><circle cx="{hx}" cy="{hy}" r="4.5" fill="{BG}"/>')
add(f'<rect x="{DCONN_X - 11}" y="{DPIN_Y0 - 18}" width="22" height="{7 * DPIN_PITCH + 36}" rx="3" '
    f'fill="#efe9d8" stroke="#bfb69c"/>')
# screen: bezel, then rPod's main menu (Classic theme) at 1:1
SX, SY = DM_X + DM_W - 335, DM_Y + 35
add(f'<rect x="{SX - 8}" y="{SY - 8}" width="336" height="256" rx="4" fill="#0d0d0f"/>')
add(f'<rect x="{SX}" y="{SY}" width="320" height="240" fill="#fff"/>')
text(SX + 10, SY + 22, "10:24", 15, "start", "bold", "#000")
text(SX + 160, SY + 22, "rPod", 17, "middle", "bold", "#000")
add(f'<rect x="{SX + 282}" y="{SY + 9}" width="24" height="13" rx="2" fill="none" stroke="#000" stroke-width="1.6"/>'
    f'<rect x="{SX + 306}" y="{SY + 12.5}" width="3" height="6" fill="#000"/>'
    f'<rect x="{SX + 284.5}" y="{SY + 11.5}" width="15" height="8" fill="#000"/>')
add(f'<rect x="{SX}" y="{SY + 32}" width="320" height="1.5" fill="#000"/>')
for i, item in enumerate(("Music", "Now Playing", "Settings", "Extras")):
    ry = SY + 36 + i * 34
    if i == 0:
        add(f'<rect x="{SX}" y="{ry}" width="320" height="34" fill="#000"/>')
    c = "#fff" if i == 0 else "#000"
    text(SX + 12, ry + 24, item, 19, "start", "bold", c)
    add(f'<path d="M{SX + 300},{ry + 10} l7,7 l-7,7" fill="none" stroke="{c}" stroke-width="2.6"/>')

# ---- Pi Zero 2 W ---------------------------------------------------------
add(f'<g filter="url(#shadow)"><rect x="{BX}" y="{BY}" width="{BW}" height="{BH}" rx="{3 * S}" '
    f'fill="url(#pcb)" stroke="#11512d" stroke-width="2"/></g>')
for hx, hy in ((3.5, 3.5), (61.5, 3.5), (3.5, 26.5), (61.5, 26.5)):
    cx, cy = m(hx, hy)
    add(f'<circle cx="{cx}" cy="{cy}" r="{2.6 * S}" fill="#d4af37" fill-opacity=".9"/>'
        f'<circle cx="{cx}" cy="{cy}" r="{1.375 * S}" fill="{BG}"/>')
x0, y0 = m(-0.8, 9.5)                          # microSD, left end
add(f'<rect x="{x0}" y="{y0}" width="{12.3 * S}" height="{12 * S}" rx="4" fill="url(#metal)" stroke="#8f959b"/>')
text(x0 + 6.2 * S, y0 + 6.4 * S, "microSD", 16, "middle", "bold", "#5b6168")
x0, y0 = m(60.6, 7.5)                          # camera, right end
add(f'<rect x="{x0}" y="{y0}" width="{4.6 * S}" height="{15 * S}" rx="2" fill="#2b2b2b"/>'
    f'<rect x="{x0 + 3}" y="{y0 + 4}" width="{1.6 * S}" height="{15 * S - 8}" fill="#e8dcc0"/>')
cx, cy = m(59.4, 15)
text(cx, cy, "CAMERA", 14, "middle", "bold", "#cfe9d8", f'transform="rotate(-90 {cx} {cy})"')
x0, y0 = m(23.5, 9.5)                          # SoC
add(f'<rect x="{x0}" y="{y0}" width="{14.5 * S}" height="{13.5 * S}" rx="6" fill="url(#can)" stroke="#8f959b"/>')
text(x0 + 7.25 * S, y0 + 6.5 * S, "RP3A0", 22, "middle", "bold", "#7b8188")
text(x0 + 7.25 * S, y0 + 6.5 * S + 24, "512 MB", 16, "middle", "normal", "#7b8188")
for cxm, wmm, dmm, label in ((12.4, 11.2, 7.6, "mini HDMI"), (41.4, 8.0, 5.6, "USB"), (54.0, 8.0, 5.6, "PWR IN")):
    x0, y0 = m(cxm - wmm / 2, 30 - dmm + 0.8)
    add(f'<rect x="{x0}" y="{y0}" width="{wmm * S}" height="{dmm * S}" rx="4" fill="url(#metal)" stroke="#8f959b"/>')
    text(x0 + wmm * S / 2, BY + BH + 26, label, 16, "middle", "bold", MUTE)
cx, cy = m(48.5, 14.6)
text(cx, cy, "Raspberry Pi", 19, "middle", "bold", "#eaf6ee")
text(cx, cy + 24, "Zero 2 W", 19, "middle", "bold", "#eaf6ee")

# header base + the pins left free
add(f'<rect x="{COL1_X - PITCH / 2}" y="{BASE_TOP}" width="{20 * PITCH}" height="{2 * PITCH}" rx="3" fill="#1b1b1b"/>')
used = {p for _, p, _, _ in DISPLAY} | {p for _, p, _, _ in WHEEL}
for n in range(1, 41):
    if n not in used:
        x, y = pin_xy(n)
        add(f'<rect x="{x - 4.5}" y="{y - 4.5}" width="9" height="9" fill="#d4af37"/>')
for n in (1, 39):
    x, _ = pin_xy(n)
    text(x, BASE_BOT + 24, str(n), 17, "middle", "bold", "#eaf6ee")

# ---- wires ---------------------------------------------------------------
for i, (_, pin, colour, _) in enumerate(DISPLAY):
    wire(pin, DSLOT_X0 + i * SLOT, DPIN_Y0 + i * DPIN_PITCH, DCONN_X, +1, colour)
for j, (rpin, pin, colour, _) in enumerate(WHEEL):
    wire(pin, WSLOT_X0 + j * SLOT, bpin_y(rpin), BPIN_X, -1, colour)

# housings on the header, each with its pin number
for n in sorted(used):
    x, y = pin_xy(n)
    add(f'<rect x="{x - HOUSING / 2}" y="{y - HOUSING / 2}" width="{HOUSING}" height="{HOUSING}" rx="3" '
        f'fill="#141414" stroke="#4a4a4a" stroke-width="1.2"/>')
    text(x, y + 5, str(n), 14, "middle", "bold", "#fff")

# far ends: plugs and labels
for i, (name, pin, colour, pilabel) in enumerate(DISPLAY):
    y = DPIN_Y0 + i * DPIN_PITCH
    add(f'<rect x="{DCONN_X - 7}" y="{y - 7}" width="14" height="14" rx="2" fill="#f7f3e8" stroke="#8d8471"/>'
        f'<rect x="{DCONN_X - 3}" y="{y - 3}" width="6" height="6" fill="{colour}"/>')
    text(DCONN_X + 16, y + 5.5, name, 15, "start", "bold", "#fff")
    text(DM_X - 12, y - 7, f"pin {pin} · {pilabel}", 15, "end", "bold")
for rpin, pin, colour, pilabel in WHEEL:
    y = bpin_y(rpin)
    add(f'<rect x="{BPIN_X - 8}" y="{y - 8}" width="16" height="16" rx="2" fill="#141414" stroke="#4a4a4a"/>')
    text(BO_X + BO_W + 12, y - 7, f"pin {pin} · {pilabel}", 15, "start", "bold")


# ---- notes ---------------------------------------------------------------
def note(x, y, w, title, lines, accent):
    h = 48 + 25 * len(lines)
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="#fff" stroke="#dedbd2"/>'
        f'<rect x="{x}" y="{y}" width="5" height="{h}" rx="2.5" fill="{accent}"/>')
    text(x + 18, y + 30, title, 17, "start", "bold")
    for k, line in enumerate(lines):
        ind = 14 if line.startswith(" ") else 0
        text(x + 18 + ind, y + 57 + 25 * k, line.strip(), 15)


note(18, 1000, 350, "Not Waveshare's default pins", [
    "DC → pin 18 (GPIO 24), not 22:",
    "  GPIO 25 is the wheel's DATA.",
    "BL → pin 33 (GPIO 13, PWM1), not 12:",
    "  GPIO 18 is the I²S DAC's clock.",
    "VCC → 3V3, to match 3.3 V logic.",
], "#2a9d4b")
note(1324, 1000, 352, "Click wheel", [
    "3.3 V part: never wire it to 5 V.",
    "CLOCK and DATA use the Pi's",
    "  internal pull-ups (rpod-wheel).",
    "Ribbon pins 3 and 7 tie to 1 (3V3);",
    "  4 and 5 stay unconnected.",
], "#16a3b8")
text(48, H - 30, "Pin map: docs/PLAN.md §1.2 · boot config: system/config.txt.d/rpod.txt · "
     "any 3V3 or GND header pin works for power", 15, fill=MUTE)

add("</svg>")
with open(sys.argv[1], "w") as f:
    f.write("\n".join(out) + "\n")

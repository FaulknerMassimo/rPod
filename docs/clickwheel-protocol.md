# Click wheel protocol — derived bit positions

**Status: derived on hardware, 2026-09-30** (Pi 3B dev board, `wheel-sniff
--guided`, 249 packets, every one well-formed). `daemon/wheel_bits.h` holds
these values.

See `docs/PLAN.md` §4 for the protocol background and §4.3 for why the
reference implementation's (`dupontgu/retro-ipod-spotify-client`) bit
constants cannot be reused directly — they were empirically fitted to a
buggy bit-packing function and only make sense paired with that bug.

## How to derive

1. Wire the click wheel: CLOCK -> GPIO 23, DATA -> GPIO 25 (`docs/PLAN.md`
   §1.2). Both lines use the Pi's internal pull-ups — no external resistors
   needed.
2. Install pigpio on the Pi (`docs/PLAN.md` §4.5), then refresh the sysroot
   and build the sniffer on the dev machine:
   ```sh
   ssh rpod@rpod.local 'cd /tmp && git clone --depth 1 https://github.com/joan2937/pigpio.git && cd pigpio && make -j4 && sudo make install'
   make sysroot wheel-sniff
   scp build/wheel-sniff rpod@rpod.local:   # home dir: /tmp is tmpfs, gone on reboot
   ```
3. Run it in guided mode from a terminal on the dev machine. It prompts for
   each gesture in turn: idle, touch, each button, a slow turn each way.
   Packets go to the log, and each gesture's section is marked `# phase`:
   ```sh
   ssh -t rpod@rpod.local 'sudo ./wheel-sniff --guided > wheel-sniff.log'
   ```
   (Plain `sudo ./wheel-sniff` just streams packets until Ctrl-C.)
4. Confirm every packet begins with the same 5-bit pattern in arrival order
   (`0`, `1`, `1`, `0`, `1` — docs/PLAN.md §4.2; the first bit to arrive
   is the *rightmost* printed character). If it doesn't, this isn't a
   4th-gen wheel (§4.4), and the framing needs revisiting before anything
   below applies.
5. Diff each button's phase against the idle/touch packets to find the bit
   that flips per button, and the touch phase for touch-detected.
6. In the rotation phases, find the field that counts monotonically, and
   the value it wraps at (positions per revolution — not necessarily 256).
7. From the log's `gap=` / `dur=` columns and final `# total` line, note the
   bit period, the gap between packets, and the narrowest CLOCK pulse.
8. Record the results below and in `daemon/wheel_bits.h`.

## Derived bit positions

Bit *n* is the (*n*+1)th bit to arrive after the packet starts.

| Field | Bit | Notes |
|---|---|---|
| Center button | 8 | 1 while pressed |
| Right button (NEXT) | 9 | |
| Left button (PREV) | 10 | |
| Down button (PLAY/PAUSE) | 11 | |
| Up button (MENU) | 12 | |
| Wheel touched | 30 | 1 while a finger is on the ring |
| Wheel position | 16–22 (shift 16, mask `0x7F`) | 0 at the top (MENU), increasing clockwise |
| Positions per revolution | 96 | values 0–95; bit 23 always 0 |
| Preamble | low byte `0x1A`, bit 31 set | arrival order 0,1,0,1,1,0,0,0 |

Constant in all 249 packets: ones `0x8000001A`, zeros `0x3F80E0E5`. Only
bits 8–12, 16–22 and 30 ever changed.

Why the reference's constants look "off by one": its `setBit()` lands every
bit one position low (`docs/PLAN.md` §4.3). Its buttons 7–11 and touch 29 are
the real 8–12 and 30 here, and its `0b01101` preamble check is the real
`0x1A` shifted down by one. Its position read, `(bits >> 16) & 0xFF` of the
shifted packing, is real bits 17–24, so it silently dropped the position's
low bit.

| Timing | Value |
|---|---|
| Bit period | ~18.2 µs (565 µs for the 31 periods of a packet, very steady) |
| Narrowest CLOCK pulse | 7 µs high, 7 µs low (sampled at 1 µs) |
| Gap between packets | ≥ 14.4 ms (so ≤ ~70 packets/s) |

## Behaviour worth knowing

- **Packets are sent only on change**, not repeated while a button is held
  or a finger rests on the ring. Resting a finger for several seconds
  produced 5 packets; each press gave exactly one packet down and one up.
  Every packet carries the full state, so a missed packet is corrected by
  the next one. There is no need, and no basis, for a timeout that
  "releases" a quiet button: that would break long-press.
- **The position field is noise while untouched.** It changes between
  packets with bit 30 clear (e.g. center presses), so only diff positions
  between two touched packets.
- **Pressing MENU/PLAY/PREV/NEXT also registers as a touch** near that edge
  of the ring, with ±1 position of jitter. Scroll logic must not turn that
  into a step (it's well under the 6 positions per step in
  `src/input/wheel_input.c`).
- **The clock runs well past each packet.** ~75 rising edges per packet
  against 32 packet bits: the extra bits are idle 1s, which §4.2's
  32-ones idle rule absorbs.
- A slow, deliberate full turn ran at ~35 positions/s, one position per
  packet.

## Hardware confirmed

- Generation: consistent with 4th-gen — fixed 32-bit framing and a constant
  preamble on every packet (`docs/PLAN.md` §4.4).
- Hold-switch polarity assumed by `daemon/rpod-wheel.c`'s `hold_engaged()`:
  active-low — GPIO 16 pulled to GND when hold is engaged, held high by the
  internal pull-up otherwise. Not wired yet; verify against the physical
  switch and fix `hold_engaged()` if the wiring says otherwise.

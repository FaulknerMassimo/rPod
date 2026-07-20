# Click wheel protocol — derived bit positions

**Status: not yet derived.** `daemon/wheel_bits.h` refuses to compile until
this file has real data and its `#error` line has been replaced with the
constants documented below.

See `docs/PLAN.md` §4 for the protocol background and §4.3 for why the
reference implementation's (`dupontgu/retro-ipod-spotify-client`) bit
constants cannot be reused directly — they were empirically fitted to a
buggy bit-packing function and only make sense paired with that bug.

## How to derive

1. Wire the click wheel: CLOCK -> GPIO 23, DATA -> GPIO 25 (`docs/PLAN.md`
   §1.2). Both lines use the Pi's internal pull-ups — no external resistors
   needed.
2. Build and run the sniffer on hardware:
   ```sh
   make wheel-sniff
   scp build/wheel-sniff rpod@rpod.local:
   ssh rpod@rpod.local 'sudo ./wheel-sniff'
   ```
3. Confirm every packet begins with the same 5-bit pattern in arrival order
   (`0`, `1`, `1`, `0`, `1` — docs/PLAN.md §4.2). If it doesn't, this isn't a
   4th-gen wheel (§4.4) and the framing algorithm needs revisiting before
   anything below applies.
4. Press each button in isolation (center, left, right, up, down) a few
   times each. Diff consecutive packets to find the bit that flips per
   button.
5. Touch and release the wheel surface without pressing a button or
   rotating; find the bit that toggles for touch-detected.
6. Rotate the wheel slowly through a full revolution, watching for an
   8-bit field that counts monotonically (with wraparound at 0/255).
7. Fill in the table and the header snippet below, then update
   `daemon/wheel_bits.h` to match and delete its `#error` line.

## Derived bit positions

| Field | Bit | Notes |
|---|---|---|
| Center button | TBD | |
| Left button | TBD | |
| Right button | TBD | |
| Up button | TBD | |
| Down button | TBD | |
| Wheel touched | TBD | |
| Wheel position | TBD (8 bits, shift = ?) | |

## Updating `daemon/wheel_bits.h`

Replace the `#error` line with:

```c
#define RPOD_WHEEL_BIT_CENTER   <n>
#define RPOD_WHEEL_BIT_RIGHT    <n>
#define RPOD_WHEEL_BIT_LEFT     <n>
#define RPOD_WHEEL_BIT_DOWN     <n>
#define RPOD_WHEEL_BIT_UP       <n>
#define RPOD_WHEEL_BIT_TOUCH    <n>
#define RPOD_WHEEL_POS_SHIFT    <n>
#define RPOD_WHEEL_POS_MASK     0xFFu
```

## Hardware confirmed

- Generation: TBD — confirm 4th-gen per `docs/PLAN.md` §4.4 (the sniffer
  should show the `01101` arrival-order preamble on every packet).
- Wheel-touched polarity: TBD (does the bit read 1 when touched, or 0?).
- Hold-switch polarity assumed by `daemon/rpod-wheel.c`'s `hold_engaged()`:
  active-low — GPIO 16 pulled to GND when hold is engaged, held high by the
  internal pull-up otherwise. Verify against the physical switch and fix
  `hold_engaged()` if the wiring says otherwise.

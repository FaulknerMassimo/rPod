/*
 * Field bit positions within a decoded 32-bit click wheel packet
 * (docs/PLAN.md §4.2). Included only by rpod-wheel.c.
 *
 * Derived on hardware with tools/wheel-sniff.c --guided (2026-09-30) — see
 * docs/clickwheel-protocol.md for the captures behind each value. Bit n is
 * the (n+1)th bit to arrive after the packet starts (correct `1u << index`
 * packing). Not the reference dupontgu/retro-ipod-spotify-client constants,
 * which only work paired with its buggy setBit() (docs/PLAN.md §4.3).
 */

#ifndef RPOD_WHEEL_BITS_H
#define RPOD_WHEEL_BITS_H

#define RPOD_WHEEL_BIT_CENTER   8
#define RPOD_WHEEL_BIT_RIGHT    9    /* NEXT */
#define RPOD_WHEEL_BIT_LEFT     10   /* PREV */
#define RPOD_WHEEL_BIT_DOWN     11   /* PLAY/PAUSE */
#define RPOD_WHEEL_BIT_UP       12   /* MENU */
#define RPOD_WHEEL_BIT_TOUCH    30   /* 1 while a finger is on the ring */

/* Absolute position around the ring: 0 at the top (MENU), increasing
 * clockwise, 96 positions per revolution. Meaningless while untouched. */
#define RPOD_WHEEL_POS_SHIFT    16
#define RPOD_WHEEL_POS_MASK     0x7Fu
#define RPOD_WHEEL_POS_RING     96

/* Every packet's low byte is 0x1A and its bit 31 is set. */
#define RPOD_WHEEL_PREAMBLE_MASK 0x800000FFu
#define RPOD_WHEEL_PREAMBLE      0x8000001Au

/* Bits arrive every ~18 us with no pauses inside a packet; packets are
 * >= 14 ms apart. A rising-edge gap past this abandons a half-packet. */
#define RPOD_WHEEL_RESYNC_GAP_US 150

#endif /* RPOD_WHEEL_BITS_H */

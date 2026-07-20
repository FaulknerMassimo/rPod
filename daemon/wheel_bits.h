/*
 * Field bit positions within a decoded 32-bit click wheel packet
 * (docs/PLAN.md §4.2). Included only by rpod-wheel.c — nothing else needs
 * these to build, so this is the one file that stays gated until real
 * hardware data exists.
 *
 * NOT YET DERIVED. Do not port the values from the reference
 * dupontgu/retro-ipod-spotify-client — they were empirically fitted to a
 * buggy bit-packing function (setBit() called starting at index 0, so its
 * first call evaluates `1 << -1`) and only make sense paired with that bug.
 * See docs/PLAN.md §4.3.
 *
 * Run tools/wheel-sniff.c on real hardware (press each button in isolation,
 * then rotate slowly), diff the packets, and record the findings in
 * docs/clickwheel-protocol.md. Then replace this whole file's contents with
 * the definitions documented there.
 */

#ifndef RPOD_WHEEL_BITS_H
#define RPOD_WHEEL_BITS_H

#error "Click wheel bit positions not yet derived on hardware — see docs/PLAN.md §4.3 and docs/clickwheel-protocol.md"

/* #define RPOD_WHEEL_BIT_CENTER   <n> */
/* #define RPOD_WHEEL_BIT_RIGHT    <n> */
/* #define RPOD_WHEEL_BIT_LEFT     <n> */
/* #define RPOD_WHEEL_BIT_DOWN     <n> */
/* #define RPOD_WHEEL_BIT_UP       <n> */
/* #define RPOD_WHEEL_BIT_TOUCH    <n> */
/* #define RPOD_WHEEL_POS_SHIFT    <n> */
/* #define RPOD_WHEEL_POS_MASK     0xFFu */

#endif /* RPOD_WHEEL_BITS_H */

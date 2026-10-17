/*
 * The battery gauge's arithmetic, kept apart from the hardware so `make
 * test` can run it: the PiSugar 2's IP5209 registers decoded, and a stream
 * of readings turned into what the UI shows -- a steady percentage, and
 * whether it's on USB power.
 *
 * The IP5209 is a power bank chip, not a fuel gauge: it measures the cell's
 * voltage but keeps no charge count, and its current register is what the
 * Pi draws, not what goes into or out of the cell (it doesn't change with
 * USB power in, and the voltage doesn't follow it). So the percentage comes
 * from the voltage alone, through PiSugar's discharge curve for this board
 * (pisugar-power-manager-rs, pisugar-core/src/ip5209.rs), averaged over
 * half a minute and only ever stepping one way while the power source stays
 * the same, so it doesn't wander -- and while charging, which pushes the
 * voltage up ahead of the charge, climbing no faster than a cell charges.
 * USB power has a status register of its own.
 *
 * See docs/PLAN.md §7.6 and src/power/battery.h, which feeds this.
 */

#ifndef RPOD_GAUGE_H
#define RPOD_GAUGE_H

#include <stdbool.h>
#include <stdint.h>

/* The IP5209 on I2C, and the registers read: the cell's voltage and the
 * current the Pi draws, each a 14-bit two's-complement number split
 * low/high, and USB power. */
#define RPOD_IP5209_ADDR     0x75
#define RPOD_IP5209_VBAT_LO  0xa2
#define RPOD_IP5209_VBAT_HI  0xa3
#define RPOD_IP5209_LOAD_LO  0xa4
#define RPOD_IP5209_LOAD_HI  0xa5
/* Not in PiSugar's driver: found on the device, 0x15 with USB power in and
 * 0 without, switching within a second of the plug. */
#define RPOD_IP5209_USB      0x78

/* The cell's voltage in mV (about 0.27 mV a step, above 2.6 V). */
int rpod_ip5209_voltage_mv(uint8_t lo, uint8_t hi);

/* The current in mA (about 0.75 mA a step), negative: what the Pi draws.
 * Only logged. */
int rpod_ip5209_current_ma(uint8_t lo, uint8_t hi);

/* PiSugar's curve: the cell's voltage under a Pi's load -> 0-100 %. */
int rpod_gauge_level_for_mv(int mv);

/* Below this, sustained, the device powers itself off before the PiSugar
 * cuts out under it (see rpod_gauge_t.critical). */
#define RPOD_GAUGE_CRITICAL_LEVEL 3

typedef struct {
    /* What the UI shows. */
    int level;     /* 0-100; -1 until the first reading */
    bool external; /* USB power in */
    bool charging; /* external, and charging -- for now, always */
    bool critical; /* on battery at or under RPOD_GAUGE_CRITICAL_LEVEL for a while */
    int mv;        /* the last voltage reading */

    /* Internal. */
    double avg_mv;    /* averaged, less what the charger holds it up by */
    uint32_t rise_ms; /* on USB power: since the level last climbed */
    uint32_t low_ms;  /* how long it's been at the critical level */
} rpod_gauge_t;

void rpod_gauge_init(rpod_gauge_t *g);

/* Feeds one reading -- the cell's voltage, and whether USB power is in --
 * `dt_ms` after the previous one (ignored for the first). Returns whether
 * anything the UI shows changed. */
bool rpod_gauge_feed(rpod_gauge_t *g, int mv, bool usb, uint32_t dt_ms);

#endif /* RPOD_GAUGE_H */

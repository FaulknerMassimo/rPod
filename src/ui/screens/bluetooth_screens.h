/*
 * Settings > Bluetooth (docs/PLAN.md §6.3, §8.1), on top of audio/bluetooth.h:
 *
 *   Bluetooth            On/Off -- adapter power
 *   <paired devices>     Connected / Not Connected  >  Connect or Disconnect,
 *                                                     Forget This Device
 *   Search for Devices   >  live list of nearby headphones/speakers; select
 *                           one to pair + trust + connect it
 *
 * Every screen rebuilds from the BlueZ mirror whenever it changes, keeping
 * the wheel's highlight on the same row. Switching MPD over to the headset
 * is still Settings > Audio Output.
 */

#ifndef RPOD_BLUETOOTH_SCREENS_H
#define RPOD_BLUETOOTH_SCREENS_H

#include "screen_stack.h"

/* build_fn for rpod_screen_stack_push(); takes no ctx (pass NULL, NULL). */
void rpod_bluetooth_screen_build(rpod_screen_stack_t *stack, lv_obj_t *screen, void *ctx);

#endif /* RPOD_BLUETOOTH_SCREENS_H */

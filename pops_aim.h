#ifndef POPS_AIM_H
#define POPS_AIM_H

#include <psptypes.h>

/* GunCon emulation for POPS (PS1) games.
 *
 * The controller firmware sends an absolute aim position normalized to
 * 0..65535 per axis. While it does, POPS's controller port 1 is presented
 * to the game as a Namco GunCon carrying that position, and a crosshair is
 * drawn on the PSP screen. See pops_aim.c. */

/* Locate POPS's controller port state. Safe to call repeatedly; returns 1
 * once found. Call from a normal thread, inside POPS. */
int  pops_aim_find_pad_state(void);
/* Latest aim from the controller: (x << 16) | y, 0..65535 each. */
void pops_aim_set(u32 packed_xy);
/* Latest position from a real GunCon: (x << 16) | y in GunCon units,
 * written to the game unchanged. */
void pops_aim_set_raw(u32 packed_xy);
/* The controller points off screen (a light gun sees no light). */
void pops_aim_set_offscreen(void);
/* Aiming stopped: port 1 goes back to the digital pad. */
void pops_aim_release(void);
/* Present port 1 as a GunCon at the current aim, or restore the pad. Call
 * from the sceCtrl hooks (POPS polls the pad through them). */
void pops_aim_apply(void);
/* Remote buttons about to be given to POPS, as the game should see them.
 * With a real gun the trigger is held back while the screen is flashed to
 * find where a shot at a dark spot went (see pops_aim.c). Call from the
 * sceCtrl hooks. */
unsigned int pops_aim_filter_buttons(unsigned int buttons);
/* Remote button state, called periodically (elapsed_ms since the last
 * call). Holding START + CROSS (GunCon A + B) toggles the crosshair. */
void pops_aim_buttons(unsigned int buttons, int elapsed_ms);
/* Stop the on-screen crosshair thread (module cleanup). */
void pops_aim_shutdown(void);

#endif

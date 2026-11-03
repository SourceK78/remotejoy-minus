/*
 * pops_aim.c - GunCon emulation for POPS
 *
 * POPS (pops_XXg.prx) emulates the PS1 controller serial protocol byte by
 * byte. Each port has a 0x30-byte state block in PSP scratchpad RAM at
 * gp + 0x3c00 + port * 0x30:
 *   +0x00..  data bytes returned after the header (0-1 = buttons)
 *   +0x21    port enabled (1)
 *   +0x22    controller ID byte returned first (0x41 = digital pad)
 *   +0x23    number of data bytes (2 for the digital pad)
 *   +0x2c    second header byte (0x5A)
 * POPS writes the ID and length once at start-up and never again, and at
 * each poll it only refreshes the two button bytes, right after reading the
 * PSP pad through sceCtrlPeekBufferNegative - the call this plugin hooks.
 *
 * So while the controller firmware sends gyro aim, port 1 is turned into a
 * Namco GunCon (ID 0x63, 6 data bytes: buttons, X, Y) from inside that hook,
 * and any GunCon-capable PS1 game sees a real light gun. No per-game patch
 * is needed. The digital pad comes back as soon as aiming stops.
 */
#include <pspkernel.h>
#include <pspinit.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspreg.h>
#include <string.h>

#include "pops_aim.h"
#include "rjm_log.h"

/* Where POPS shows the PS1 picture on the 480x272 PSP screen. The default
 * is POPS's "Original" screen mode: a 320x240 picture at 1:1, centered. */
#ifndef RJM_POPS_AIM_SCREEN_X
#define RJM_POPS_AIM_SCREEN_X 80
#endif
#ifndef RJM_POPS_AIM_SCREEN_Y
#define RJM_POPS_AIM_SCREEN_Y 16
#endif
#ifndef RJM_POPS_AIM_SCREEN_W
#define RJM_POPS_AIM_SCREEN_W 320
#endif
#ifndef RJM_POPS_AIM_SCREEN_H
#define RJM_POPS_AIM_SCREEN_H 240
#endif
#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

/* TV out screen size (XMB: Connected Display Settings > Screen Size), read
 * from the system registry. With 16:9 the PS1 picture sits pillarboxed in
 * the middle of the TV frame; with 4:3 it fills the frame's width, so no
 * horizontal correction is applied. RJM_POPS_AIM_TV_4_3=0/1 forces it. */
#ifndef RJM_POPS_AIM_TV_ASPECT_DIR
#define RJM_POPS_AIM_TV_ASPECT_DIR "/CONFIG/DISPLAY"
#endif
#ifndef RJM_POPS_AIM_TV_ASPECT_KEY
#define RJM_POPS_AIM_TV_ASPECT_KEY "aspect_ratio"
#endif
/* Value of that key meaning 16:9 (confirmed on hardware); any other value
 * is the only other choice, 4:3. */
#ifndef RJM_POPS_AIM_TV_ASPECT_16_9
#define RJM_POPS_AIM_TV_ASPECT_16_9 0
#endif

/* POPS port state (see the top of this file). Every pops_XXg.prx generation
 * (01g-11g) has $gp = 0x00010000, i.e. PSP scratchpad RAM, and the same
 * port layout; so the block sits at a fixed address that is always safe to
 * read, and no module lookup is needed. */
#define POPS_GP            0x00010000u
#define PAD_STATE_BASE     0x3c00u
#define PAD_ENABLED        0x21
#define PAD_ID             0x22
#define PAD_LENGTH         0x23
#define PAD_HEADER2        0x2c
#define PAD_ID_DIGITAL     0x41
#define PAD_LENGTH_DIGITAL 2
#define PAD_ID_GUNCON      0x63
#define PAD_LENGTH_GUNCON  6

/* POPS's HOME-menu screen size, in scratchpad at gp + 0x3663. POPS
 * refreshes it every frame while showing on the LCD (all 7 generations,
 * 01g-11g, use the same address and the same scaling table at the code's
 * 0xFED30-equivalent). Values: 0 Original, 1 Normal, 2 Zoom, 3 Full, 4 same
 * scaling as Original. */
#define POPS_SCREEN_MODE   0x3663u

/* GunCon coordinates: X in 8 MHz clocks since hsync, Y in scanlines. These
 * are the theoretical edges of the PS1 picture: 2560 GPU clocks wide
 * (53.69 MHz), i.e. 2560 * 8 / 53.69 = 381.4 GunCon units, and 240 lines
 * tall (NTSC lines 16..256), centred on X 269, Y 136. Measured values vary
 * with the display and with how precisely the gun was adjusted, so any
 * remaining offset is left to the gun's own position adjustment (and the
 * crosshair can be hidden). Off screen the gun reports "no light" as X=01h,
 * Y=0Ah. */
#define GUNCON_X_MIN       78
#define GUNCON_X_MAX       460
#define GUNCON_Y_MIN       16
#define GUNCON_Y_MAX       256
#define GUNCON_X_NO_LIGHT  0x0001
#define GUNCON_Y_NO_LIGHT  0x000A

/* Port 1 state block address, 0 until found.
 *
 * The plugin starts before popsman loads pops_XXg.prx. Polling the module
 * list while POPS was being loaded powered the PSP off, so no module lookup
 * is done at all: the block is at a fixed scratchpad address (POPS_GP). It
 * is checked only once POPS itself has polled the pad through the sceCtrl
 * hooks, after a short settle time, and only accepted if its signature
 * matches (pad_state_valid). */
static volatile u32 s_pad;
static int s_detect_done;
static volatile int s_pops_polling;
static int s_settle_ticks;
#define DETECT_SETTLE_TICKS 20   /* special-thread ticks, about 1 s */
#if RJM_ENABLE_LOG
static int s_logged_module;
static int s_logged_invalid;
#endif
/* Written by the USB thread, read by the sceCtrl hooks. A single word keeps
 * X and Y consistent; the valid flag is written after the value. */
static volatile u32 s_aim_xy;
static volatile int s_aim_valid;
static volatile int s_aim_offscreen;
/* s_aim_xy holds GunCon units (from a real gun) rather than 0..65535. */
static volatile int s_aim_raw;
/* Last real-gun position with light, and when "no light" began (see
 * RJM_POPS_AIM_GUN_HOLD_MS). */
static volatile u32 s_lit_xy;
static volatile int s_lit_valid;
static volatile u32 s_no_light_since;
/* Counts lit real-gun readings, so a reading newer than a moment can be told
 * apart from an old one. */
static volatile u32 s_lit_seq;
/* Size of the PS1 picture relative to the whole display, Q16. On the LCD in
 * POPS "Original" mode the picture is 320x240 of 480x272; on TV out it is
 * 640x480 of 720x480. Set by the overlay thread, which already queries the
 * display. 1.0 means the picture fills that axis. */
static volatile u32 s_picture_width_q16 = 0x10000u;
static volatile u32 s_picture_height_q16 = 0x10000u;
/* TV out is set to 4:3 (read once, before the crosshair thread starts). */
static int s_tv_4_3;
/* Port 1 currently presented as a GunCon; set in the sceCtrl hook, read by
 * the overlay thread. */
static volatile int s_guncon_active;
/* On-screen crosshair: a GunCon game hides its own cursor. */
static SceUID s_overlay_thid = -1;
static volatile int s_overlay_run;
/* Crosshair hidden by the user (START + CROSS held, i.e. GunCon A + B). */
#define OVERLAY_TOGGLE_BUTTONS (PSP_CTRL_START | PSP_CTRL_CROSS)
#define OVERLAY_TOGGLE_HOLD_MS 2000
static volatile int s_overlay_hidden;
static int s_toggle_held_ms;
static void overlay_start(void);

/* Only plain PSP memory is ever read: the scratchpad (where POPS keeps it)
 * or main RAM. Anything else is rejected before being dereferenced. */
static int pad_address_ok(u32 pad)
{
	return (pad >= 0x00010000u && pad + 0x30u <= 0x00014000u) ||
		(pad >= 0x08800000u && pad + 0x30u <= 0x0A000000u);
}

/* The block is only touched while it looks exactly like POPS's pad state,
 * either as POPS set it up or as this module changed it. */
static int pad_state_valid(u32 pad)
{
	u8 id;
	u8 len;

	if(!pad_address_ok(pad))
	{
		return 0;
	}
	id = _lb(pad + PAD_ID);
	len = _lb(pad + PAD_LENGTH);

	if(_lb(pad + PAD_ENABLED) != 1 || _lb(pad + PAD_HEADER2) != 0x5A)
	{
		return 0;
	}
	return (id == PAD_ID_DIGITAL && len == PAD_LENGTH_DIGITAL) ||
		(id == PAD_ID_GUNCON && len == PAD_LENGTH_GUNCON);
}

int pops_aim_find_pad_state(void)
{
	u32 pad;

	if(s_detect_done)
	{
		/* The crosshair thread is only needed once aim data has arrived
		 * (never with controller firmware that sends no aim). Start it here,
		 * in a normal thread, rather than from the sceCtrl hook. */
		if(s_pad && s_aim_valid)
		{
			overlay_start();
		}
		return s_pad != 0;
	}
	if(sceKernelApplicationType() != PSP_INIT_KEYCONFIG_POPS)
	{
		s_detect_done = 1;
		return 0;
	}

	/* Wait until POPS has polled the pad through our hooks at least once. */
	if(!s_pops_polling || ++s_settle_ticks < DETECT_SETTLE_TICKS)
	{
		return 0;
	}
	pad = POPS_GP + PAD_STATE_BASE;
#if RJM_ENABLE_LOG
	if(!s_logged_module)
	{
		s_logged_module = 1;
		rjmLogText("pops aim: POPS is polling the pad\n");
	}
#endif
	if(!pad_state_valid(pad))
	{
#if RJM_ENABLE_LOG
		/* Record once what was there, in case the layout differs. */
		if(!s_logged_invalid)
		{
			s_logged_invalid = 1;
			rjmLogHex("pops aim: pad state not ready at ", (int) pad);
			rjmLogHex("pops aim:  enabled ", _lb(pad + PAD_ENABLED));
			rjmLogHex("pops aim:  id ", _lb(pad + PAD_ID));
			rjmLogHex("pops aim:  length ", _lb(pad + PAD_LENGTH));
			rjmLogHex("pops aim:  header2 ", _lb(pad + PAD_HEADER2));
		}
#endif
		return 0;
	}

	s_pad = pad;
	s_detect_done = 1;
	rjmLogHex("pops aim: POPS pad state at ", (int) pad);
	return 1;
}

void pops_aim_set(u32 packed_xy)
{
	s_aim_xy = packed_xy;
	s_aim_raw = 0;
	s_lit_valid = 0;
	s_aim_offscreen = 0;
	s_aim_valid = 1;
}

/* ------------------------------------------------------------------------
 * Shots with a real gun
 *
 * A light gun only sees where it points if that part of the screen is lit,
 * and the position reaches this module several frames after the picture was
 * shown (POPS frame, TV out, gun, controller, USB). Two things follow:
 *  - Aimed at a dark part of the screen, the gun reports "no light" and the
 *    shot misses. So when the trigger is pulled without light, the trigger is
 *    held back from the game, the screen is flashed (inverted, so dark parts
 *    turn bright) until the gun reports a position, and only then is the
 *    trigger passed on together with that position. If no light comes within
 *    RJM_POPS_AIM_FLASH_WAIT_MS, the gun points off screen and the trigger
 *    goes through with "no light" (reload).
 *  - A game that flashes the screen itself on a shot (e.g. Time Crisis) reads
 *    the gun during its own flash, before the light from it can arrive here.
 *    So for RJM_POPS_AIM_SHOT_HOLD_MS after each shot, "no light" is replaced
 *    by the shot position and the game keeps seeing where the shot went.
 * ---------------------------------------------------------------------- */
#ifndef RJM_POPS_AIM_FLASH
#define RJM_POPS_AIM_FLASH 1
#endif
/* GunCon trigger as it arrives from the controller firmware (PS circle). */
#ifndef RJM_POPS_AIM_TRIGGER
#define RJM_POPS_AIM_TRIGGER PSP_CTRL_CIRCLE
#endif
#ifndef RJM_POPS_AIM_FLASH_WAIT_MS
#define RJM_POPS_AIM_FLASH_WAIT_MS 100
#endif
#ifndef RJM_POPS_AIM_SHOT_HOLD_MS
#define RJM_POPS_AIM_SHOT_HOLD_MS 250
#endif
/* A trigger held back by the flash is pressed for at least this long, even
 * if it was already let go, so the game sees the shot. */
#ifndef RJM_POPS_AIM_SHOT_PRESS_MS
#define RJM_POPS_AIM_SHOT_PRESS_MS 50
#endif

#define SHOT_IDLE  0
#define SHOT_FLASH 1   /* trigger held back, screen flashed */
#define SHOT_HELD  2   /* trigger passed on, position held */
static volatile int s_shot_state;
static volatile u32 s_shot_xy;
static u32 s_shot_since;
static u32 s_shot_lit_seq;
static int s_shot_prev_trigger;
/* The trigger was held back, so it is pressed for the game for a while. */
static int s_shot_force_press;
/* The overlay thread flashes the screen while set. */
static volatile int s_flash_wanted;
#if RJM_ENABLE_LOG
/* Result of the last flash, logged from the overlay thread. */
static volatile int s_flash_log_pending;
static volatile u32 s_flash_log_ms;
static volatile u32 s_flash_log_xy;
#endif

static int shot_holding(u32 now)
{
	return s_shot_state == SHOT_HELD &&
		now - s_shot_since < RJM_POPS_AIM_SHOT_HOLD_MS * 1000u;
}

static void shot_fire(u32 now, u32 xy, int force_press)
{
	s_shot_force_press = force_press;
	s_shot_xy = xy;
	s_shot_since = now;
	s_shot_state = SHOT_HELD;
	s_flash_wanted = 0;
}

/* A real gun on interlaced TV out (composite) can alternate between a
 * position and "no light" in parts of the screen, which made shots miss and
 * the crosshair flicker. A short "no light" keeps the last position; only a
 * longer one (pointing off screen) is passed on. */
#ifndef RJM_POPS_AIM_GUN_HOLD_MS
#define RJM_POPS_AIM_GUN_HOLD_MS 200
#endif

/* "No light" on either axis. The gun can report it on one axis only (seen:
 * X=01h with a real Y), which is no usable position either. */
static int is_no_light(u32 xy)
{
	return (xy >> 16) == GUNCON_X_NO_LIGHT || (xy & 0xFFFFu) == GUNCON_Y_NO_LIGHT;
}

/* The real gun's position to use now (see RJM_POPS_AIM_GUN_HOLD_MS). */
static u32 gun_xy(void)
{
	u32 xy = s_aim_xy;
	if(is_no_light(xy) && shot_holding(sceKernelGetSystemTimeLow()))
	{
		return s_shot_xy;
	}
	if(is_no_light(xy) && s_lit_valid &&
		sceKernelGetSystemTimeLow() - s_no_light_since <
		RJM_POPS_AIM_GUN_HOLD_MS * 1000u)
	{
		return s_lit_xy;
	}
	if(is_no_light(xy))
	{
		return ((u32) GUNCON_X_NO_LIGHT << 16) | GUNCON_Y_NO_LIGHT;
	}
	return xy;
}

#if RJM_ENABLE_LOG
/* Once a second: the spread of the unfiltered lit readings and how many
 * readings were lit / no light, to measure the jitter. */
static u32 s_stat_start;
static u32 s_stat_min_x = 0xFFFFu, s_stat_max_x, s_stat_min_y = 0xFFFFu, s_stat_max_y;
static int s_stat_lit, s_stat_dark;

static void gun_stats(u32 xy)
{
	if(is_no_light(xy))
	{
		s_stat_dark++;
	}
	else
	{
		u32 x = xy >> 16, y = xy & 0xFFFFu;
		if(x < s_stat_min_x) s_stat_min_x = x;
		if(x > s_stat_max_x) s_stat_max_x = x;
		if(y < s_stat_min_y) s_stat_min_y = y;
		if(y > s_stat_max_y) s_stat_max_y = y;
		s_stat_lit++;
	}
}

/* Logged from the overlay thread: file writes from the USB receive path,
 * where pops_aim_set_raw runs, do not reach the log. */
static void gun_stats_flush(void)
{
	u32 now = sceKernelGetSystemTimeLow();
	if(now - s_stat_start < 1000000u || (s_stat_lit == 0 && s_stat_dark == 0))
	{
		return;
	}
	if(s_stat_lit)
	{
		rjmLogHex("gun 1s x min ", (int) s_stat_min_x);
		rjmLogHex("gun 1s x max ", (int) s_stat_max_x);
		rjmLogHex("gun 1s y min ", (int) s_stat_min_y);
		rjmLogHex("gun 1s y max ", (int) s_stat_max_y);
	}
	rjmLogHex("gun 1s lit ", s_stat_lit);
	rjmLogHex("gun 1s dark ", s_stat_dark);
	s_stat_start = now;
	s_stat_min_x = s_stat_min_y = 0xFFFFu;
	s_stat_max_x = s_stat_max_y = 0;
	s_stat_lit = s_stat_dark = 0;
}
#endif

void pops_aim_set_raw(u32 packed_xy)
{
#if RJM_ENABLE_LOG
	gun_stats(packed_xy);
#endif
	if(!is_no_light(packed_xy))
	{
		s_lit_xy = packed_xy;
		s_lit_valid = 1;
		s_lit_seq++;
	}
	else if(!s_aim_raw || !is_no_light(s_aim_xy))
	{
		s_no_light_since = sceKernelGetSystemTimeLow();
	}
	s_aim_xy = packed_xy;
	s_aim_raw = 1;
	s_aim_offscreen = 0;
	s_aim_valid = 1;
}

void pops_aim_set_offscreen(void)
{
	s_aim_raw = 0;
	s_aim_offscreen = 1;
	s_aim_valid = 1;
}

void pops_aim_release(void)
{
	s_aim_valid = 0;
	s_lit_valid = 0;
}

unsigned int pops_aim_filter_buttons(unsigned int buttons)
{
	u32 now;
	int trigger;

	if(!s_guncon_active || !s_aim_valid || !s_aim_raw || s_aim_offscreen)
	{
		s_shot_state = SHOT_IDLE;
		s_flash_wanted = 0;
		s_shot_prev_trigger = 0;
		return buttons;
	}
	now = sceKernelGetSystemTimeLow();
	trigger = (buttons & RJM_POPS_AIM_TRIGGER) != 0;

	if(trigger && !s_shot_prev_trigger && s_shot_state != SHOT_FLASH)
	{
		u32 xy = gun_xy();
		if(!is_no_light(xy) || !RJM_POPS_AIM_FLASH)
		{
			shot_fire(now, xy, 0);
		}
		else
		{
			s_shot_state = SHOT_FLASH;
			s_shot_since = now;
			s_shot_lit_seq = s_lit_seq;
			s_flash_wanted = 1;
		}
	}
	s_shot_prev_trigger = trigger;

	if(s_shot_state == SHOT_FLASH)
	{
		u32 waited = now - s_shot_since;
		if(s_lit_seq != s_shot_lit_seq)
		{
			shot_fire(now, s_lit_xy, 1);
		}
		else if(waited >= RJM_POPS_AIM_FLASH_WAIT_MS * 1000u)
		{
			shot_fire(now, ((u32) GUNCON_X_NO_LIGHT << 16) | GUNCON_Y_NO_LIGHT, 1);
		}
		else
		{
			return buttons & ~RJM_POPS_AIM_TRIGGER;
		}
#if RJM_ENABLE_LOG
		s_flash_log_ms = waited / 1000u;
		s_flash_log_xy = s_shot_xy;
		s_flash_log_pending = 1;
#endif
	}
	else if(s_shot_state == SHOT_HELD && !shot_holding(now))
	{
		s_shot_state = SHOT_IDLE;
	}

	if(s_shot_state == SHOT_HELD && s_shot_force_press &&
		now - s_shot_since < RJM_POPS_AIM_SHOT_PRESS_MS * 1000u)
	{
		buttons |= RJM_POPS_AIM_TRIGGER;
	}
	return buttons;
}

#if RJM_ENABLE_LOG
static u32 correct_raw(u32 raw);
#endif

void pops_aim_buttons(unsigned int buttons, int elapsed_ms)
{
#if RJM_ENABLE_LOG
	/* GunCon calibration: on each new button press (the trigger), log where
	 * a real gun points, as received and as given to the game. Shooting the
	 * edges of the PS1 picture gives the range the crosshair should use. */
	static unsigned int prev_buttons;
	if((buttons & ~prev_buttons) && s_guncon_active && s_aim_valid && s_aim_raw)
	{
		u32 raw = s_aim_xy;
		u32 fixed = correct_raw(gun_xy());
		rjmLogHex("gun shot raw x ", (int) (raw >> 16));
		rjmLogHex("gun shot raw y ", (int) (raw & 0xFFFFu));
		rjmLogHex("gun shot game x ", (int) (fixed >> 16));
		rjmLogHex("gun shot game y ", (int) (fixed & 0xFFFFu));
		rjmLogHex("gun shot picture w q16 ", (int) s_picture_width_q16);
		rjmLogHex("gun shot picture h q16 ", (int) s_picture_height_q16);
	}
	prev_buttons = buttons;
#endif
	/* Only while presenting a GunCon, so normal pad play is unaffected. A
	 * hold toggles once; the buttons must be released before the next. */
	if(!s_guncon_active ||
		(buttons & OVERLAY_TOGGLE_BUTTONS) != OVERLAY_TOGGLE_BUTTONS)
	{
		s_toggle_held_ms = 0;
		return;
	}
	if(s_toggle_held_ms < 0)
	{
		return;
	}
	s_toggle_held_ms += elapsed_ms;
	if(s_toggle_held_ms >= OVERLAY_TOGGLE_HOLD_MS)
	{
		s_overlay_hidden = !s_overlay_hidden;
		s_toggle_held_ms = -1;
	}
}

/* GunCon units to 0..65535 over the assumed GunCon edges, clamped. Used
 * only to place the crosshair for a real gun. */
static u32 unscale_axis(u16 v, u16 lo, u16 hi)
{
	if(v <= lo)
	{
		return 0;
	}
	if(v >= hi)
	{
		return 0xFFFFu;
	}
	return ((u32) (v - lo) * 0xFFFFu) / (u32) (hi - lo);
}

static u32 raw_to_normalized(u32 raw)
{
	return (unscale_axis((u16) (raw >> 16), GUNCON_X_MIN, GUNCON_X_MAX) << 16) |
		unscale_axis((u16) raw, GUNCON_Y_MIN, GUNCON_Y_MAX);
}

/* Stretch one GunCon axis about the centre of its range by 1 / ratio:
 *   V' = C + (V - C) / ratio. 32-bit only (no libgcc 64-bit division in this
 * module): GunCon values are a few hundred at most, so clamp before scaling
 * to rule out overflow. */
static int stretch_axis(int v, int lo, int hi, u32 ratio)
{
	int centre = (lo + hi) / 2;

	/* ratio < 1: picture smaller than the display (stretch); ratio > 1: the
	 * picture overflows the display (Zoom) and is compressed. */
	if(ratio == 0x10000u || ratio == 0)
	{
		return v;
	}
	if(v > 2047)
	{
		v = 2047;
	}
	v = centre + ((v - centre) * 0x10000) / (int) ratio;
	if(v < 0)
	{
		v = 0;
	}
	if(v > 0xFFFF)
	{
		v = 0xFFFF;
	}
	return v;
}

/* A real gun (e.g. a GunCon 3 adapter) reports its position across the
 * whole screen, but POPS shows the PS1 picture in only part of it. Each
 * axis is stretched about the centre of the GunCon range by the picture's
 * share of the display (s_picture_width_q16 / s_picture_height_q16), so the
 * edges of the PS1 picture map to the GunCon edges. "No light" (X=01h,
 * Y=0Ah) passes through unchanged. */
static u32 correct_raw(u32 raw)
{
	int x = (int) (raw >> 16);
	int y = (int) (raw & 0xFFFFu);

	if(x == GUNCON_X_NO_LIGHT && y == GUNCON_Y_NO_LIGHT)
	{
		return raw;
	}
	x = stretch_axis(x, GUNCON_X_MIN, GUNCON_X_MAX, s_picture_width_q16);
	y = stretch_axis(y, GUNCON_Y_MIN, GUNCON_Y_MAX, s_picture_height_q16);
	return ((u32) x << 16) | (u32) y;
}

static u16 scale_axis(u32 norm, u16 lo, u16 hi)
{
	/* 0..65535 onto lo..hi without ever exceeding hi. */
	return (u16) (lo + ((norm * ((u32) (hi - lo) + 1)) >> 16));
}

/* Called from the sceCtrl hooks. For POPS this runs inside its port-1 poll,
 * after the PSP pad read and before the game receives the ID byte. */
void pops_aim_apply(void)
{
	u32 pad = s_pad;
	u16 x;
	u16 y;

	if(!pad)
	{
		/* Tell detection that the game is running and polling the pad. */
		s_pops_polling = 1;
		return;
	}

	if(!s_aim_valid)
	{
		/* Aiming stopped: give the game its digital pad back. */
		if(s_guncon_active)
		{
			if(pad_state_valid(pad))
			{
				_sb(PAD_ID_DIGITAL, pad + PAD_ID);
				_sb(PAD_LENGTH_DIGITAL, pad + PAD_LENGTH);
			}
			s_guncon_active = 0;
		}
		return;
	}

	if(!pad_state_valid(pad))
	{
		s_guncon_active = 0;
		return;
	}

	if(s_aim_offscreen)
	{
		x = GUNCON_X_NO_LIGHT;
		y = GUNCON_Y_NO_LIGHT;
	}
	else if(s_aim_raw)
	{
		/* A real gun: its coordinates go to the game as they are, except
		 * for the correction to the PS1 picture area. */
		u32 xy = correct_raw(gun_xy());
		x = (u16) (xy >> 16);
		y = (u16) xy;
	}
	else
	{
		u32 xy = s_aim_xy;
		x = scale_axis(xy >> 16, GUNCON_X_MIN, GUNCON_X_MAX);
		y = scale_axis(xy & 0xFFFFu, GUNCON_Y_MIN, GUNCON_Y_MAX);
	}

	/* Data bytes 2-5 follow the buttons, little endian; POPS never writes
	 * them. */
	_sb((u8) x, pad + 2);
	_sb((u8) (x >> 8), pad + 3);
	_sb((u8) y, pad + 4);
	_sb((u8) (y >> 8), pad + 5);
	_sb(PAD_ID_GUNCON, pad + PAD_ID);
	_sb(PAD_LENGTH_GUNCON, pad + PAD_LENGTH);
	s_guncon_active = 1;
}

/* ------------------------------------------------------------------------
 * On-screen crosshair
 *
 * A small DuckStation-style crosshair: a centre dot and four arms with a gap,
 * white with a one-pixel black outline so it stays visible on any
 * background. It is drawn straight into the frame buffer POPS is showing,
 * only while GunCon aim is active and on screen, with the background kept so
 * it can be erased where POPS does not repaint (see overlay_step).
 * ---------------------------------------------------------------------- */
#define XH_SIZE 13
#define XH_HALF 6

/* '#' = white. The outline is derived from it. */
static const char k_crosshair[XH_SIZE][XH_SIZE + 1] =
{
	"......#......",
	"......#......",
	"......#......",
	"......#......",
	".............",
	".............",
	"####..#..####",
	".............",
	".............",
	"......#......",
	"......#......",
	"......#......",
	"......#......",
};

static int xh_white(int x, int y)
{
	return x >= 0 && y >= 0 && x < XH_SIZE && y < XH_SIZE && k_crosshair[y][x] == '#';
}

/* Colours written for the crosshair in a given pixel format. */
static u32 xh_color(int fmt, int white)
{
	if(fmt == PSP_DISPLAY_PIXEL_FORMAT_8888)
	{
		return white ? 0xFFFFFFFFu : 0xFF000000u;
	}
	return white ? 0xFFFFu : (fmt == PSP_DISPLAY_PIXEL_FORMAT_565 ? 0x0000u : 0x8000u);
}

/* Crosshair cell (x, y) in -1..XH_SIZE: 1 = white, 0 = outline, -1 = none. */
static int xh_cell(int x, int y)
{
	if(xh_white(x, y))
	{
		return 1;
	}
	if(xh_white(x - 1, y) || xh_white(x + 1, y) || xh_white(x, y - 1) || xh_white(x, y + 1))
	{
		return 0;
	}
	return -1;
}

#define XH_BOX (XH_SIZE + 2)
#define FLASH_PROBE_GRID 4
#define FLASH_PROBES (FLASH_PROBE_GRID * FLASH_PROBE_GRID)

/* What was drawn into one frame buffer, so it can be checked and erased.
 * POPS may alternate between buffers, so a few are tracked. */
struct XhRecord
{
	u32 base;       /* uncached frame buffer address, 0 = unused */
	int stride;
	int fmt;
	int drawn;      /* crosshair currently expected in this buffer */
	int w, h;       /* displayed size (480x272 on the LCD, larger on TV out) */
	int cx, cy;
	u32 saved[XH_BOX * XH_BOX];  /* background under the crosshair */
	int inverted;   /* flashed (see flash_invert) */
	u32 probe[FLASH_PROBES];     /* pixels read right after the flash */
};

#define XH_RECORDS 3
static struct XhRecord s_xh[XH_RECORDS];
static int s_xh_next;

static int xh_addr(const struct XhRecord *r, int px, int py, u32 *addr)
{
	if(px < 0 || py < 0 || px >= r->w || py >= r->h || px >= r->stride)
	{
		return 0;
	}
	*addr = r->base + (u32) (py * r->stride + px) * (r->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2);
	return 1;
}

static u32 xh_read(const struct XhRecord *r, u32 addr)
{
	return r->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? _lw(addr) : (u32) _lh(addr);
}

static void xh_write(const struct XhRecord *r, u32 addr, u32 value)
{
	if(r->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888)
	{
		_sw(value, addr);
	}
	else
	{
		_sh((u16) value, addr);
	}
}

/* Whether every crosshair pixel in the buffer still holds our colour. */
static int xh_intact(const struct XhRecord *r)
{
	int x, y;
	u32 addr;
	u32 mask = r->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 0xFFFFFFFFu : 0xFFFFu;
	for(y = -1; y <= XH_SIZE; y++)
	{
		for(x = -1; x <= XH_SIZE; x++)
		{
			int cell = xh_cell(x, y);
			if(cell < 0 || !xh_addr(r, r->cx - XH_HALF + x, r->cy - XH_HALF + y, &addr))
			{
				continue;
			}
			if((xh_read(r, addr) & mask) != xh_color(r->fmt, cell))
			{
				return 0;
			}
		}
	}
	return 1;
}

/* Erase the crosshair pixel by pixel: a pixel that still holds our colour
 * gets its saved background back, one POPS has repainted is left alone. POPS
 * repaints only the PS1 picture, so the part of the crosshair that reached
 * into the border around it is erased here instead of staying as a trail. */
static void xh_erase(struct XhRecord *r)
{
	int x, y;
	u32 addr;
	u32 mask = r->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 0xFFFFFFFFu : 0xFFFFu;
	for(y = -1; y <= XH_SIZE; y++)
	{
		for(x = -1; x <= XH_SIZE; x++)
		{
			int cell = xh_cell(x, y);
			if(cell < 0 || !xh_addr(r, r->cx - XH_HALF + x, r->cy - XH_HALF + y, &addr))
			{
				continue;
			}
			if((xh_read(r, addr) & mask) == xh_color(r->fmt, cell))
			{
				xh_write(r, addr, r->saved[(y + 1) * XH_BOX + (x + 1)]);
			}
		}
	}
	r->drawn = 0;
}

static void xh_draw(struct XhRecord *r, int cx, int cy)
{
	int x, y;
	u32 addr;
	r->cx = cx;
	r->cy = cy;
	for(y = -1; y <= XH_SIZE; y++)
	{
		for(x = -1; x <= XH_SIZE; x++)
		{
			int cell = xh_cell(x, y);
			if(cell < 0 || !xh_addr(r, cx - XH_HALF + x, cy - XH_HALF + y, &addr))
			{
				continue;
			}
			r->saved[(y + 1) * XH_BOX + (x + 1)] = xh_read(r, addr);
			xh_write(r, addr, xh_color(r->fmt, cell));
		}
	}
	r->drawn = 1;
}

static struct XhRecord *xh_record_for(u32 base, int stride, int fmt, int w, int h)
{
	int i;
	struct XhRecord *r;
	for(i = 0; i < XH_RECORDS; i++)
	{
		if(s_xh[i].base == base)
		{
			r = &s_xh[i];
			if(r->stride != stride || r->fmt != fmt || r->w != w || r->h != h)
			{
				/* Layout changed: whatever we drew is gone or meaningless. */
				r->stride = stride;
				r->fmt = fmt;
				r->w = w;
				r->h = h;
				r->drawn = 0;
				r->inverted = 0;
			}
			return r;
		}
	}
	r = &s_xh[s_xh_next];
	s_xh_next = (s_xh_next + 1) % XH_RECORDS;
	r->base = base;
	r->stride = stride;
	r->fmt = fmt;
	r->w = w;
	r->h = h;
	r->drawn = 0;
	r->inverted = 0;
	return r;
}

#if RJM_POPS_AIM_FLASH
/* Shot flash (see "Shots with a real gun"). The colour bits of the whole
 * shown frame are inverted: dark parts turn bright for the gun, and doing it
 * again gives the frame back exactly, so nothing has to be saved. A frame
 * POPS repaints in the meantime is inverted again while the flash lasts and
 * left alone afterwards; probe pixels tell whether it was repainted. */
static u32 flash_mask(int fmt)
{
	switch(fmt)
	{
		case PSP_DISPLAY_PIXEL_FORMAT_8888: return 0x00FFFFFFu;
		case PSP_DISPLAY_PIXEL_FORMAT_5551: return 0x7FFFu;
		case PSP_DISPLAY_PIXEL_FORMAT_4444: return 0x0FFFu;
		default: return 0xFFFFu;
	}
}

static u32 flash_probe_addr(const struct XhRecord *r, int i)
{
	u32 addr = 0;
	int px = ((i % FLASH_PROBE_GRID) * 2 + 1) * r->w / (FLASH_PROBE_GRID * 2);
	int py = ((i / FLASH_PROBE_GRID) * 2 + 1) * r->h / (FLASH_PROBE_GRID * 2);
	xh_addr(r, px, py, &addr);
	return addr;
}

static int flash_intact(const struct XhRecord *r)
{
	int i;
	for(i = 0; i < FLASH_PROBES; i++)
	{
		u32 addr = flash_probe_addr(r, i);
		if(addr && xh_read(r, addr) != r->probe[i])
		{
			return 0;
		}
	}
	return 1;
}

static void flash_invert(struct XhRecord *r)
{
	int x, y, i;
	u32 mask = flash_mask(r->fmt);

	for(y = 0; y < r->h; y++)
	{
		if(r->fmt == PSP_DISPLAY_PIXEL_FORMAT_8888)
		{
			volatile u32 *p = (volatile u32 *) (r->base + (u32) (y * r->stride) * 4);
			for(x = 0; x < r->w; x++)
			{
				p[x] ^= mask;
			}
		}
		else if((r->stride & 1) == 0)
		{
			/* Two pixels per word; an odd width also flips the next pixel
			 * of the same row (stride > width), which is undone the same
			 * way. */
			volatile u32 *p = (volatile u32 *) (r->base + (u32) (y * r->stride) * 2);
			u32 mask2 = mask | (mask << 16);
			for(x = 0; x < (r->w + 1) / 2; x++)
			{
				p[x] ^= mask2;
			}
		}
		else
		{
			volatile u16 *p = (volatile u16 *) (r->base + (u32) (y * r->stride) * 2);
			for(x = 0; x < r->w; x++)
			{
				p[x] ^= (u16) mask;
			}
		}
	}
	for(i = 0; i < FLASH_PROBES; i++)
	{
		u32 addr = flash_probe_addr(r, i);
		r->probe[i] = addr ? xh_read(r, addr) : 0;
	}
	r->inverted = !r->inverted;
}

/* Flash over: undo it in every frame POPS has not repainted since. */
static void flash_restore_all(void)
{
	int i;
	for(i = 0; i < XH_RECORDS; i++)
	{
		struct XhRecord *r = &s_xh[i];
		if(!r->inverted)
		{
			continue;
		}
		if(flash_intact(r))
		{
			flash_invert(r);
		}
		r->inverted = 0;
	}
}
#endif

/* Where the PS1 picture is on a display of w x h pixels. On the LCD it is the
 * configured rectangle. On a larger display (TV out) POPS enlarges the
 * picture by a whole factor per axis and centres it (720x480: 640x480 at
 * 40,0; interlaced 672x240: 640x240 at 16,0). Build with
 * RJM_POPS_AIM_TV_X/Y/W/H to use a measured rectangle instead. */
/* PS1 picture placement on the 480x272 LCD for each POPS screen mode, from
 * POPS's own scaling table (horizontal scale /256 of the 2560-dot-clock
 * line, vertical scale /256 of 240 lines, top offset; always centred
 * horizontally). Zoom is taller than the screen and is cropped. */
static const short k_lcd_rect[5][4] =
{
	{ 80, 16, 320, 240 },   /* 0 Original: 1:1 */
	{ 60, 1, 360, 270 },    /* 1 Normal: 4:3, 288/256 vertical */
	{ 0, -44, 480, 360 },   /* 2 Zoom: x1.5, top/bottom cropped */
	{ 0, 0, 480, 272 },     /* 3 Full: stretched */
	{ 80, 16, 320, 240 },   /* 4 same table entry as Original */
};

static void picture_rect(int w, int h, int *px, int *py, int *pw, int *ph)
{
	if(w <= PSP_SCREEN_W && h <= PSP_SCREEN_H)
	{
		u8 mode = s_pad ? _lb(POPS_GP + POPS_SCREEN_MODE) : 0xFF;
		if(mode < sizeof(k_lcd_rect) / sizeof(k_lcd_rect[0]))
		{
			*px = k_lcd_rect[mode][0];
			*py = k_lcd_rect[mode][1];
			*pw = k_lcd_rect[mode][2];
			*ph = k_lcd_rect[mode][3];
			return;
		}
		/* Mode unknown (POPS not found yet): the configured placement. */
		*px = RJM_POPS_AIM_SCREEN_X;
		*py = RJM_POPS_AIM_SCREEN_Y;
		*pw = RJM_POPS_AIM_SCREEN_W;
		*ph = RJM_POPS_AIM_SCREEN_H;
		return;
	}
#if defined(RJM_POPS_AIM_TV_X) && defined(RJM_POPS_AIM_TV_Y) && \
	defined(RJM_POPS_AIM_TV_W) && defined(RJM_POPS_AIM_TV_H)
	*px = RJM_POPS_AIM_TV_X;
	*py = RJM_POPS_AIM_TV_Y;
	*pw = RJM_POPS_AIM_TV_W;
	*ph = RJM_POPS_AIM_TV_H;
#else
	{
		/* Whole-number scale per axis, centred: 672x480 (progressive) gives
		 * 640x480 at 16,0, and 672x240 (interlaced, e.g. composite) gives
		 * 640x240 at 16,0 - POPS still doubles the width there. */
		int sx = w / RJM_POPS_AIM_SCREEN_W;
		int sy = h / RJM_POPS_AIM_SCREEN_H;
		if(sx < 1)
		{
			sx = 1;
		}
		if(sy < 1)
		{
			sy = 1;
		}
		*pw = RJM_POPS_AIM_SCREEN_W * sx;
		*ph = RJM_POPS_AIM_SCREEN_H * sy;
		*px = (w - *pw) / 2;
		*py = (h - *ph) / 2;
	}
#endif
	if(s_tv_4_3)
	{
		/* 4:3 TV: the picture spans the whole width, no X correction. */
		*px = 0;
		*pw = w;
	}
}

#if RJM_ENABLE_LOG
/* Log builds: every few seconds, log the bounding box of non-black pixels in
 * the shown frame, i.e. where the picture really is (TV out calibration). */
static void overlay_log_picture(u32 base, int stride, int fmt, int w, int h)
{
	static int ticks;
	static int last_x0 = -1, last_y0, last_x1, last_y1;
	int x, y, x0 = w, y0 = h, x1 = -1, y1 = -1;
	int bpp = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
	u32 mask = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 0x00FFFFFFu :
		(fmt == PSP_DISPLAY_PIXEL_FORMAT_565 ? 0xFFFFu : 0x7FFFu);

	if(++ticks < 3000)
	{
		return;
	}
	ticks = 0;
	for(y = 0; y < h; y++)
	{
		for(x = 0; x < w; x++)
		{
			u32 addr = base + (u32) (y * stride + x) * bpp;
			u32 v = bpp == 4 ? _lw(addr) : (u32) _lh(addr);
			if(v & mask)
			{
				if(x < x0) x0 = x;
				if(x > x1) x1 = x;
				if(y < y0) y0 = y;
				if(y > y1) y1 = y;
			}
		}
	}
	if(x1 < 0 || (x0 == last_x0 && y0 == last_y0 && x1 == last_x1 && y1 == last_y1))
	{
		return;
	}
	last_x0 = x0;
	last_y0 = y0;
	last_x1 = x1;
	last_y1 = y1;
	rjmLogHex("overlay picture x0 ", x0);
	rjmLogHex("overlay picture y0 ", y0);
	rjmLogHex("overlay picture x1 ", x1);
	rjmLogHex("overlay picture y1 ", y1);
}
#endif

#if RJM_ENABLE_LOG
/* Log the display layout whenever it changes, so the picture placement on
 * TV out can be checked. */
static void overlay_log_layout(u32 base, int stride, int fmt, int w, int h)
{
	static int last_stride, last_fmt, last_w, last_h;
	/* The buffer address alternates every frame, so it does not trigger a
	 * log line on its own (that would write the file 60 times a second). */
	if(stride == last_stride && fmt == last_fmt && w == last_w && h == last_h)
	{
		return;
	}
	last_stride = stride;
	last_fmt = fmt;
	last_w = w;
	last_h = h;
	rjmLogHex("overlay fb ", (int) base);
	rjmLogHex("overlay stride ", stride);
	rjmLogHex("overlay fmt ", fmt);
	rjmLogHex("overlay width ", w);
	rjmLogHex("overlay height ", h);
}
#endif

/* TV out mode watch.
 * sceDisplaySetMode stores the new size at once but the new mode only after
 * waiting for the switch, so a read during a switch pairs the old mode with
 * the new size. Only a reading that stays the same for ~50 ms counts.
 * 0xD1 is TV 480i (LCDC 858x525, encoder interlaced); 0xE1 is the same
 * 13.5MHz timing with 263 lines and the encoder in non-interlaced mode,
 * which POPS already uses for the game picture. */
#if RJM_ENABLE_LOG
static int s_tv_seen_mode = -1, s_tv_seen_w, s_tv_seen_h;
static int s_tv_stable_mode = -1, s_tv_stable_w, s_tv_stable_h;
static u32 s_tv_seen_since;

static void tv_mode_watch(void)
{
	int mode, w, h;
	u32 now = sceKernelGetSystemTimeLow();
	if(sceDisplayGetMode(&mode, &w, &h) < 0)
	{
		return;
	}
	if(mode != s_tv_seen_mode || w != s_tv_seen_w || h != s_tv_seen_h)
	{
		s_tv_seen_mode = mode;
		s_tv_seen_w = w;
		s_tv_seen_h = h;
		s_tv_seen_since = now;
		return;
	}
	if(now - s_tv_seen_since < 50000u ||
		(mode == s_tv_stable_mode && w == s_tv_stable_w && h == s_tv_stable_h))
	{
		return;
	}
	s_tv_stable_mode = mode;
	s_tv_stable_w = w;
	s_tv_stable_h = h;
	rjmLogHex("tv mode ", mode);
	rjmLogHex("tv mode width ", w);
	rjmLogHex("tv mode height ", h);
}
#endif

/* One overlay step on the buffer currently being shown: when the crosshair
 * moved, is no longer wanted, or was partly repainted by POPS, erase what is
 * left of it (xh_erase), then draw it where it is wanted. A static screen
 * such as a logo is never repainted by POPS, so erasing is what prevents
 * trails there and in the border around the picture. */
static void overlay_step(void)
{
	void *top = NULL;
	int stride = 0;
	int fmt = 0;
	int want;
	int cx = 0, cy = 0;
	int mode = 0, w = PSP_SCREEN_W, h = PSP_SCREEN_H;
	struct XhRecord *r;
	u32 base;

	if(sceDisplayGetFrameBuf(&top, &stride, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 ||
		top == NULL)
	{
		return;
	}
	/* TV out shows a larger picture (e.g. 720x480) than the LCD. */
	if(sceDisplayGetMode(&mode, &w, &h) < 0 || w <= 0 || h <= 0)
	{
		w = PSP_SCREEN_W;
		h = PSP_SCREEN_H;
	}
	if(stride < w)
	{
		return;
	}
	/* Work through the uncached mirror so the display sees it at once. */
	base = ((u32) top & 0x1FFFFFFFu) | 0x40000000u;
#if RJM_ENABLE_LOG
	overlay_log_layout(base, stride, fmt, w, h);
	overlay_log_picture(base, stride, fmt, w, h);
#endif
	r = xh_record_for(base, stride, fmt, w, h);
	{
		int px, py, pw, ph;
		picture_rect(w, h, &px, &py, &pw, &ph);
		s_picture_width_q16 = pw > 0 ? ((u32) pw << 16) / (u32) w : 0x10000u;
		s_picture_height_q16 = ph > 0 ? ((u32) ph << 16) / (u32) h : 0x10000u;
	}

#if RJM_POPS_AIM_FLASH
	if(s_flash_wanted)
	{
		/* No crosshair during the flash; a frame not yet flashed (new, or
		 * repainted by POPS) is flashed now. */
		if(r->drawn)
		{
			xh_erase(r);
		}
		if(!r->inverted || !flash_intact(r))
		{
			r->inverted = 0;
			flash_invert(r);
		}
		return;
	}
	flash_restore_all();
#endif

	want = s_guncon_active && s_aim_valid && !s_aim_offscreen && !s_overlay_hidden;
	if(want && s_aim_raw)
	{
		/* Real gun: hide while it sees no light, otherwise map its units onto
		 * the screen with the assumed GunCon edges (clamped). */
		u32 raw = gun_xy();
		u16 gx = (u16) (raw >> 16);
		u16 gy = (u16) raw;
		if(gx == GUNCON_X_NO_LIGHT && gy == GUNCON_Y_NO_LIGHT)
		{
			want = 0;
		}
	}
	if(want)
	{
		u32 xy = s_aim_raw ? raw_to_normalized(correct_raw(gun_xy())) : s_aim_xy;
		int px, py, pw, ph;
		picture_rect(w, h, &px, &py, &pw, &ph);
		cx = px + (int) (((xy >> 16) * (u32) pw) >> 16);
		cy = py + (int) (((xy & 0xFFFFu) * (u32) ph) >> 16);
	}

	if(r->drawn && (!want || cx != r->cx || cy != r->cy || !xh_intact(r)))
	{
		xh_erase(r);
	}
	if(want && !r->drawn)
	{
		xh_draw(r, cx, cy);
	}
}

static int overlay_thread(SceSize args, void *argp)
{
	(void) args;
	(void) argp;
	/* Check about once a millisecond: POPS repaints the frame at times of its
	 * own choosing, so the crosshair is put back shortly after being
	 * overwritten instead of only once per vertical blank. */
	while(s_overlay_run)
	{
		overlay_step();
#if RJM_ENABLE_LOG
		tv_mode_watch();
		gun_stats_flush();
		if(s_flash_log_pending)
		{
			s_flash_log_pending = 0;
			rjmLogHex("gun flash wait ms ", (int) s_flash_log_ms);
			rjmLogHex("gun flash x ", (int) (s_flash_log_xy >> 16));
			rjmLogHex("gun flash y ", (int) (s_flash_log_xy & 0xFFFFu));
		}
#endif
		sceKernelDelayThread(1000);
	}
	sceKernelExitDeleteThread(0);
	return 0;
}

#if !defined(RJM_POPS_AIM_TV_4_3)
static void reg_open(REGHANDLE *h, int *ok)
{
	struct RegParam reg;

	memset(&reg, 0, sizeof(reg));
	reg.regtype = 1;
	reg.namelen = strlen(SYSTEM_REGISTRY);
	reg.unk2 = 1;
	reg.unk3 = 1;
	strcpy(reg.name, SYSTEM_REGISTRY);
	*ok = sceRegOpenRegistry(&reg, 1, h) >= 0;
}

static int reg_get_int(REGHANDLE hd, const char *name, int *value)
{
	REGHANDLE hk;
	unsigned int type;
	SceSize size;

	return sceRegGetKeyInfo(hd, name, &hk, &type, &size) >= 0 &&
		type == REG_TYPE_INT && size == 4 &&
		sceRegGetKeyValue(hd, hk, value, 4) >= 0;
}
#endif

/* Read the TV out screen size setting (see RJM_POPS_AIM_TV_ASPECT_KEY). */
static void read_tv_aspect(void)
{
#if defined(RJM_POPS_AIM_TV_4_3)
	s_tv_4_3 = RJM_POPS_AIM_TV_4_3;
#else
	REGHANDLE h, hd;
	int ok;
	int value;

	reg_open(&h, &ok);
	if(!ok)
	{
		rjmLogText("pops aim: registry not available\n");
		return;
	}
	if(sceRegOpenCategory(h, RJM_POPS_AIM_TV_ASPECT_DIR, 1, &hd) >= 0)
	{
		if(reg_get_int(hd, RJM_POPS_AIM_TV_ASPECT_KEY, &value))
		{
			s_tv_4_3 = value != RJM_POPS_AIM_TV_ASPECT_16_9;
			rjmLogHex("pops aim: tv aspect key ", value);
		}
		sceRegCloseCategory(hd);
	}
	sceRegCloseRegistry(h);
#endif
	rjmLogHex("pops aim: tv 4:3 ", s_tv_4_3);
}

#if !defined(RJM_POPS_AIM_TV_4_3)
static int tv_aspect_thread(SceSize args, void *argp)
{
	(void) args;
	(void) argp;
	read_tv_aspect();
	return 0;
}
#endif

/* The registry is read in a short-lived thread with its own stack: the
 * caller (the special thread) has only 2 KiB. */
static void read_tv_aspect_safely(void)
{
#if defined(RJM_POPS_AIM_TV_4_3)
	read_tv_aspect();
#else
	SceUID thid = sceKernelCreateThread("RJMAimTvAspect", tv_aspect_thread,
		16, 0x4000, 0, NULL);
	if(thid < 0)
	{
		rjmLogHex("pops aim: tv aspect thread ", thid);
		return;
	}
	if(sceKernelStartThread(thid, 0, NULL) >= 0)
	{
		sceKernelWaitThreadEnd(thid, NULL);
	}
	sceKernelDeleteThread(thid);
#endif
}

/* Log builds write files from the crosshair thread, which needs more stack. */
#if RJM_ENABLE_LOG
#define OVERLAY_STACK_SIZE 0x1000
#else
#define OVERLAY_STACK_SIZE 0x800
#endif

static void overlay_start(void)
{
	static int tried;

	/* Called every special-thread tick once aim is active: try only once,
	 * so a failed thread creation is not retried (and logged) forever. */
	if(tried)
	{
		return;
	}
	tried = 1;
	read_tv_aspect_safely();
	s_overlay_run = 1;
	/* High priority so the crosshair lands right after the buffer flip,
	 * before the display scans down to it. */
	s_overlay_thid = sceKernelCreateThread("RJMAimOverlay", overlay_thread, 12,
		OVERLAY_STACK_SIZE, 0, NULL);
	if(s_overlay_thid >= 0)
	{
		sceKernelStartThread(s_overlay_thid, 0, NULL);
	}
	rjmLogHex("pops aim: overlay thread ", s_overlay_thid);
}

void pops_aim_shutdown(void)
{
	s_overlay_run = 0;
	if(s_overlay_thid >= 0 && s_overlay_thid != sceKernelGetThreadId())
	{
		sceKernelTerminateDeleteThread(s_overlay_thid);
	}
	s_overlay_thid = -1;
}

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
#include <pspdisplay.h>
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

/* GunCon coordinates: X in dot-clock units, Y in scanlines, typical NTSC
 * values for the visible picture. Games calibrate the gun themselves (for
 * example by shooting a target at start). Off screen the gun reports "no
 * light" as X=01h, Y=0Ah. */
#define GUNCON_X_MIN       77
#define GUNCON_X_MAX       461
#define GUNCON_Y_MIN       25
#define GUNCON_Y_MAX       248
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
/* Port 1 currently presented as a GunCon; set in the sceCtrl hook, read by
 * the overlay thread. */
static volatile int s_guncon_active;
/* On-screen crosshair: a GunCon game hides its own cursor. */
static SceUID s_overlay_thid = -1;
static volatile int s_overlay_run;
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
	s_aim_offscreen = 0;
	s_aim_valid = 1;
}

void pops_aim_set_offscreen(void)
{
	s_aim_offscreen = 1;
	s_aim_valid = 1;
}

void pops_aim_release(void)
{
	s_aim_valid = 0;
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
	return r;
}

/* Where the PS1 picture is on a display of w x h pixels. On the LCD it is the
 * configured rectangle. On a larger display (TV out) POPS enlarges the
 * picture: it is taken as scaled uniformly by the largest factor that fits
 * (720x480: x2, 640x480 at 40,0) and centred. Build with
 * RJM_POPS_AIM_TV_X/Y/W/H to use a measured rectangle instead. */
static void picture_rect(int w, int h, int *px, int *py, int *pw, int *ph)
{
	if(w <= PSP_SCREEN_W && h <= PSP_SCREEN_H)
	{
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
		/* Scale in 1/16 steps: min(w / W, h / H). */
		int sx = w * 16 / RJM_POPS_AIM_SCREEN_W;
		int sy = h * 16 / RJM_POPS_AIM_SCREEN_H;
		int s = sx < sy ? sx : sy;
		*pw = RJM_POPS_AIM_SCREEN_W * s / 16;
		*ph = RJM_POPS_AIM_SCREEN_H * s / 16;
		*px = (w - *pw) / 2;
		*py = (h - *ph) / 2;
	}
#endif
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

	want = s_guncon_active && s_aim_valid && !s_aim_offscreen;
	if(want)
	{
		u32 xy = s_aim_xy;
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
		sceKernelDelayThread(1000);
	}
	sceKernelExitDeleteThread(0);
	return 0;
}

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
	s_overlay_run = 1;
	/* High priority so the crosshair lands right after the buffer flip,
	 * before the display scans down to it. */
	s_overlay_thid = sceKernelCreateThread("RJMAimOverlay", overlay_thread, 12, 0x800, 0, NULL);
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

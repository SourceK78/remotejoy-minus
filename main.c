/*
 * remotejoy-minus standalone USB PRX
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspctrl_kernel.h>
#include <pspimpose_driver.h>
#include <pspinit.h>
#include <pspiofilemgr_kernel.h>
#include <psploadcore.h>
#include <pspmodulemgr.h>
#include <psppower.h>
#include <pspsdk.h>
#include <pspsysmem.h>
#include <psputilsforkernel.h>
#include <string.h>
#include <systemctrl.h>
#include "remotejoy.h"
#include "remotejoy_minus.h"
#include "rjm_log.h"

PSP_MODULE_INFO("RemoteJoyMinus", PSP_MODULE_KERNEL, 1, 1);

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define GET_JUMP_TARGET(x) (0x80000000 | (((x) & 0x03FFFFFF) << 2))
#define REMOTE_HOME_BUTTON PSP_CTRL_HOME
#define REMOTE_SOUND_BUTTON PSP_CTRL_NOTE
#define REMOTE_VIRTUAL_L2 PSP_CTRL_VOLDOWN
#define REMOTE_VIRTUAL_R2 PSP_CTRL_VOLUP
#define REMOTE_INTERNAL_BUTTONS (REMOTE_HOME_BUTTON | REMOTE_SOUND_BUTTON | REMOTE_VIRTUAL_L2 | REMOTE_VIRTUAL_R2)
#define REMOTE_IMPOSE_BUTTONS (REMOTE_INTERNAL_BUTTONS)
#define REMOTE_VOLUME_COMBO_BUTTONS (REMOTE_VIRTUAL_L2 | REMOTE_VIRTUAL_R2)
#define REMOTE_DISPLAY_COMBO_BUTTON PSP_CTRL_LEFT
#define REMOTE_SPECIAL_BUTTONS (REMOTE_INTERNAL_BUTTONS | PSP_CTRL_START | REMOTE_DISPLAY_COMBO_BUTTON)
#ifndef RJM_POPS_GATE_SLOT2_RETURN
#define RJM_POPS_GATE_SLOT2_RETURN 4
#endif
#ifndef RJM_POPS_GATE_FORCE_SLOT2_ASM
#define RJM_POPS_GATE_FORCE_SLOT2_ASM 0
#endif
#define ANALOG_CENTER 0x80
#define PSP_VOLUME_MIN 0
#define PSP_VOLUME_MAX 30
#define PSP_BRIGHTNESS_MIN 0
#define PSP_BRIGHTNESS_MAX 3
#define PSP_EQUALIZER_MIN 0
#define PSP_EQUALIZER_MAX 4
#define SPECIAL_POLL_US 50000
#define SPECIAL_POLL_MS (SPECIAL_POLL_US / 1000)
#define VOLUME_REPEAT_DELAY_MS 400
#define VOLUME_REPEAT_INTERVAL_MS 120
#define SOUND_HOLD_MS 900
#define DISPLAY_HOLD_MS 900
#define REBOOT_LOAD_MODULE_AFTER "/kd/usersystemlib.prx"
#define REBOOT_LOAD_MAX_PRX_SIZE (256 * 1024)

/*
 * POPS (pops_XXg.prx) player-2 injection.
 *
 * The hook opens POPS's second PS1 controller poll path, then detects that
 * port-B read by the caller's $s0 slot pointer. The returned sceCtrl buffer is
 * based on a real hardware read and only the game-relevant buttons/analog
 * fields are replaced with g_p2joy.
 */
#define POPS_MODULE_NAME "pops"
/* FUN_0002c718-equivalent offsets, one per sampled POPS generation. */
#define POPS_GATE_OFFSET_COUNT 5
#if RJM_ENABLE_POPS_2P
static const unsigned int g_pops_gate_offsets[POPS_GATE_OFFSET_COUNT] =
{
	0x2c718, /* 01g */
	0x2cd44, /* 02g, 03g */
	0x2cdbc, /* 04g */
	0x2cdd4, /* 07g, 09g */
	0x2c794  /* 11g */
};
#endif
/* Verified relocation-free (pure register-op) words at +0x1c from the gate
 * function's entry, identical across all 7 generations - used to sanity
 * check a candidate address before patching it. */
#define POPS_GATE_SIG_OFFSET 0x1c
#define POPS_GATE_SIG0 0x2ca30004u /* sltiu v1,a1,0x4 */
#define POPS_GATE_SIG1 0x24020004u /* li v0,0x4 */
#define POPS_GATE_SIG2 0x0043280au /* movz a1,v0,v1 */
#define POPS_PAD_SLOT_STRIDE 0x30
#define POPS_PAD_SLOT_BASE 0x3c00
#define POPS_PAD_SLOT_P2 1
/*
 * POPS expects system-level bits from real sceCtrl reads, so port-B
 * substitution keeps the original buffer and overrides only the game-relevant
 * fields.
 */
#define POPS_P2_GAME_BUTTONS_MASK \
	(PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_RIGHT | \
	PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | \
	PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE)

static SceCtrlData g_currjoy;
static SceCtrlData g_p2joy;
static int g_p2_connected;
static unsigned int g_remote_latch_make;
static unsigned int g_remote_latch_break;
static int (*g_ctrl_common)(SceCtrlData *, int count, int type);
static int (*g_ctrl_peek_positive_common)(SceCtrlData *, int count);
static int (*g_ctrl_peek_negative_common)(SceCtrlData *, int count);
static int (*g_ctrl_read_positive_common)(SceCtrlData *, int count);
static int (*g_ctrl_read_negative_common)(SceCtrlData *, int count);
static int (*g_ctrl_peek_latch_common)(SceCtrlLatch *);
static int (*g_ctrl_read_latch_common)(SceCtrlLatch *);
static unsigned int g_ctrl_peek_positive_target;
static unsigned int g_ctrl_peek_negative_target;
static unsigned int g_ctrl_read_positive_target;
static unsigned int g_ctrl_read_negative_target;
static unsigned int g_ctrl_peek_latch_target;
static unsigned int g_ctrl_read_latch_target;
static unsigned int g_ctrl_peek_positive_trampoline[5] __attribute__((aligned(16)));
static unsigned int g_ctrl_peek_negative_trampoline[5] __attribute__((aligned(16)));
static unsigned int g_ctrl_read_positive_trampoline[5] __attribute__((aligned(16)));
static unsigned int g_ctrl_read_negative_trampoline[5] __attribute__((aligned(16)));
static unsigned int g_ctrl_peek_latch_trampoline[5] __attribute__((aligned(16)));
static unsigned int g_ctrl_read_latch_trampoline[5] __attribute__((aligned(16)));
static unsigned int g_intercept_buttons;
static int g_special_thread_run;
static SceUID g_special_thid = -1;
static SceUID g_main_thid = -1;
static SceUID g_power_thid = -1;
static SceUID g_power_cbid = -1;
static int g_power_slot = -1;
static int g_virtual_analog_active;
static unsigned char g_virtual_lx;
static unsigned char g_virtual_ly;
static unsigned int g_input_reset_serial;
static int g_cleanup_started;
static int g_is_pops_context;
#if RJM_REBOOT_LOAD_POPS
static SceUID g_reboot_prx_block = -1;
#endif
#if RJM_ENABLE_POPS_2P
#define POPS_GATE_SLOT2_PULSE_MS SPECIAL_POLL_MS
unsigned int g_pops_gate_data_base;
static unsigned int g_pops_gate_target;
unsigned int g_pops_gate_trampoline[5] __attribute__((aligned(16)));
static int g_pops_gate_state; /* 0 = not tried, 1 = installed, -1 = gave up */
static int g_pops_gate_mode; /* 0 = original, 1 = fast return4, 2 = slot2 asm */
static int g_pops_gate_virtual_active;
static int g_pops_gate_slot2_pulse_ms;
static unsigned int g_pops_p2_marker; /* expected caller $s0 when port B polls */
/* Not static: pops_hook.S writes this directly via %hi/%lo addressing, which
 * needs external linkage to resolve the symbol at link time. */
volatile unsigned int g_pops_caller_s0;
static volatile unsigned int g_pops_p2_hits;
volatile unsigned int g_pops_gate_slot1_hits;
volatile unsigned int g_pops_gate_slot2_hits;
volatile unsigned int g_pops_gate_result_counts[5];
#endif

unsigned int psplinkSetK1(unsigned int k1);
void sceImposeHomeButton(int open);
int sceImposeGetStatus(void);

static void update_system_button_intercepts(unsigned int buttons);
static void reset_remote_input_state(void);
static unsigned int calc_mask(void);
static void cleanup_module(const char *reason);
#if RJM_ENABLE_POPS_2P
static void try_install_pops_gate_hook(void);
static void remove_pops_gate_hook(void);
static void update_pops_gate_mode(void);
/* Defined in pops_hook.S: captures the caller's $s0 (see comment near
 * g_pops_p2_marker's use below) before jumping into ctrl_peek_negative_hook,
 * which must therefore have external linkage too. */
extern void pops_ctrl_peek_negative_entry(void);
extern void pops_gate_entry(void);
extern int ctrl_peek_negative_hook(SceCtrlData *pad_data, int count);
#endif

static int is_pops_context(void)
{
	return g_is_pops_context;
}

static int should_yield_to_pops_context(void)
{
#if RJM_ENABLE_POPS_2P
	if(g_is_pops_context)
	{
		return 0;
	}

	return sceKernelApplicationType() == PSP_INIT_KEYCONFIG_POPS ||
		sceKernelFindModuleByName("scePops_Manager") != NULL ||
		sceKernelFindModuleByName("pops") != NULL ||
		sceKernelFindModuleByName("popsman") != NULL;
#else
	return 0;
#endif
}

#if RJM_REBOOT_LOAD_POPS
static int reserve_pops_reboot_from_path(const char *path)
{
	SceUID fd;
	int size;
	int read_total;
	void *buf;

	rjmLogText("reboot-load try path\n");
	fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	rjmLogHex("reboot-load open ", fd);
	if(fd < 0)
	{
		return fd;
	}

	size = sceIoLseek32(fd, 0, 2);
	rjmLogHex("reboot-load size ", size);
	sceIoLseek32(fd, 0, 0);
	if(size <= 0 || size > REBOOT_LOAD_MAX_PRX_SIZE)
	{
		sceIoClose(fd);
		return -1;
	}

	g_reboot_prx_block = sceKernelAllocPartitionMemory(
		PSP_MEMORY_PARTITION_KERNEL, "RJMRebootPrx", PSP_SMEM_Low, size, NULL);
	rjmLogHex("reboot-load alloc ", g_reboot_prx_block);
	if(g_reboot_prx_block < 0)
	{
		sceIoClose(fd);
		return g_reboot_prx_block;
	}

	buf = sceKernelGetBlockHeadAddr(g_reboot_prx_block);
	read_total = sceIoRead(fd, buf, size);
	sceIoClose(fd);
	rjmLogHex("reboot-load read ", read_total);
	if(read_total != size)
	{
		return -1;
	}

	sctrlHENLoadModuleOnReboot(REBOOT_LOAD_MODULE_AFTER, buf, size, BOOTLOAD_POPS);
	rjmLogText("reboot-load reserved BOOTLOAD_POPS\n");
	return 0;
}

static void reserve_pops_reboot_load(void)
{
	int keyconfig = sceKernelApplicationType();
	int apitype = sceKernelInitApitype();
	int ret;

	rjmLogHex("reboot-load keyconfig ", keyconfig);
	rjmLogHex("reboot-load apitype ", apitype);
	if(keyconfig == PSP_INIT_KEYCONFIG_POPS)
	{
		rjmLogText("reboot-load already POPS\n");
		return;
	}
	if(g_reboot_prx_block >= 0)
	{
		rjmLogText("reboot-load already reserved\n");
		return;
	}

	ret = reserve_pops_reboot_from_path("ms0:/seplugins/remotejoy-minus.prx");
	if(ret == 0)
	{
		return;
	}
	ret = reserve_pops_reboot_from_path("ef0:/seplugins/remotejoy-minus.prx");
	if(ret == 0)
	{
		return;
	}
	rjmLogHex("reboot-load failed ", ret);
}
#else
static void reserve_pops_reboot_load(void)
{
}
#endif

static unsigned int sanitize_remote_buttons(unsigned int buttons)
{
	unsigned int internal_buttons = REMOTE_INTERNAL_BUTTONS;

	if((buttons & PSP_CTRL_START) && (buttons & REMOTE_DISPLAY_COMBO_BUTTON))
	{
		buttons &= ~(PSP_CTRL_START | REMOTE_DISPLAY_COMBO_BUTTON);
	}
	if((buttons & PSP_CTRL_START) && (buttons & REMOTE_VOLUME_COMBO_BUTTONS))
	{
		buttons &= ~PSP_CTRL_START;
	}
	if(is_pops_context())
	{
		internal_buttons &= ~REMOTE_HOME_BUTTON;
	}
	buttons &= ~internal_buttons;

	return buttons;
}

static unsigned int get_remote_inject_buttons(void)
{
	unsigned int buttons = sanitize_remote_buttons(g_currjoy.Buttons);
	int k1;

	asm __volatile__ ( "move %0, $k1" : "=r"(k1) );
	if(k1)
	{
		buttons &= ~calc_mask();
	}

	return buttons;
}

static int usb_module_loaded(void)
{
	u32 addr = sctrlHENFindFunction("sceUSB_Driver", "sceUsb", 0xAE5DE6AF);
	rjmLogHex("usb_module_loaded sceUsbStart ", addr);
	return addr != 0;
}

static void load_usb_module(void)
{
	typedef int (*module_load_function_t)(const char *, int, SceKernelLMOption *);
	typedef int (*module_start_function_t)(SceUID, int, void **, int *, void *);
	module_load_function_t kernel_load_module;
	module_start_function_t kernel_start_module;
	SceUID modid;
	int status;

	if(usb_module_loaded())
	{
		rjmLogText("usb.prx already loaded\n");
		return;
	}

	kernel_load_module = (module_load_function_t)
		sctrlHENFindFunction("sceModuleManager", "ModuleMgrForKernel", 0x977DE386);
	kernel_start_module = (module_start_function_t)
		sctrlHENFindFunction("sceModuleManager", "ModuleMgrForKernel", 0x50F0C1EC);

	if(kernel_load_module == NULL || kernel_start_module == NULL)
	{
		rjmLogText("module loader functions missing\n");
		return;
	}

	modid = kernel_load_module("flash0:/kd/usb.prx", 0, NULL);
	rjmLogHex("kernel_load usb.prx ", modid);
	if(modid < 0)
	{
		return;
	}

	rjmLogHex("kernel_start usb.prx ", kernel_start_module(modid, 0, NULL, &status, NULL));
	rjmLogHex("kernel_start status ", status);
}

static int map_axis(int real, int new)
{
	int val1 = real - 127;
	int val2 = new - 127;

	return ABS(val1) > ABS(val2) ? real : new;
}

static unsigned int calc_mask(void)
{
	int i;
	unsigned int mask = 0;

	for(i = 0; i < 32; i++)
	{
		if(sceCtrlGetButtonMask(1 << i) == 1)
		{
			mask |= (1 << i);
		}
	}

	return mask;
}

static int clamp_int(int value, int min_value, int max_value)
{
	if(value < min_value)
	{
		return min_value;
	}
	if(value > max_value)
	{
		return max_value;
	}
	return value;
}

static void set_impose_param_delta(SceImposeParam param, int delta, int min_value, int max_value)
{
	int value = sceImposeGetParam(param);

	if(value < 0)
	{
		return;
	}

	sceImposeSetParam(param, clamp_int(value + delta, min_value, max_value));
}

static void cycle_impose_param(SceImposeParam param, int min_value, int max_value)
{
	int value = sceImposeGetParam(param);

	if(value < 0)
	{
		return;
	}
	value++;
	if(value > max_value)
	{
		value = min_value;
	}
	sceImposeSetParam(param, value);
}

static void toggle_impose_param(SceImposeParam param)
{
	int value = sceImposeGetParam(param);

	if(value >= 0)
	{
		sceImposeSetParam(param, value ? 0 : 1);
	}
}

static void handle_impose_buttons(unsigned int buttons)
{
	unsigned int k1;

	buttons &= REMOTE_IMPOSE_BUTTONS;
	if(buttons == 0)
	{
		return;
	}

	k1 = psplinkSetK1(0);
	if(buttons & PSP_CTRL_VOLDOWN)
	{
		set_impose_param_delta(PSP_IMPOSE_MAIN_VOLUME, -1, PSP_VOLUME_MIN, PSP_VOLUME_MAX);
	}
	if(buttons & PSP_CTRL_VOLUP)
	{
		set_impose_param_delta(PSP_IMPOSE_MAIN_VOLUME, 1, PSP_VOLUME_MIN, PSP_VOLUME_MAX);
	}
	if(buttons & PSP_CTRL_NOTE)
	{
		cycle_impose_param(PSP_IMPOSE_EQUALIZER_MODE, PSP_EQUALIZER_MIN, PSP_EQUALIZER_MAX);
	}
	if(buttons & PSP_CTRL_HOME)
	{
		sceImposeHomeButton(sceImposeGetStatus() & 1);
	}
	psplinkSetK1(k1);
}

static void handle_sound_hold(void)
{
	unsigned int k1 = psplinkSetK1(0);
	toggle_impose_param(PSP_IMPOSE_MUTE);
	psplinkSetK1(k1);
}

static unsigned int get_current_buttons(void)
{
	unsigned int buttons;
	int intc = pspSdkDisableInterrupts();
	buttons = g_currjoy.Buttons;
	pspSdkEnableInterrupts(intc);
	return buttons;
}

static void reset_remote_input_state(void)
{
	int intc = pspSdkDisableInterrupts();
	g_currjoy.Buttons = 0;
	g_currjoy.Lx = ANALOG_CENTER;
	g_currjoy.Ly = ANALOG_CENTER;
	g_p2joy.Buttons = 0;
	g_p2joy.Lx = ANALOG_CENTER;
	g_p2joy.Ly = ANALOG_CENTER;
	g_p2_connected = 0;
	g_remote_latch_make = 0;
	g_remote_latch_break = 0;
	g_virtual_analog_active = 0;
	g_virtual_lx = ANALOG_CENTER;
	g_virtual_ly = ANALOG_CENTER;
	g_input_reset_serial++;
	pspSdkEnableInterrupts(intc);

	update_system_button_intercepts(0);
}

static void calc_virtual_analog(unsigned int buttons, int *active,
	unsigned char *lx, unsigned char *ly)
{
	*active = 0;
	*lx = ANALOG_CENTER;
	*ly = ANALOG_CENTER;

	if((buttons & PSP_CTRL_START) == 0)
	{
		if((buttons & REMOTE_VOLUME_COMBO_BUTTONS) == REMOTE_VOLUME_COMBO_BUTTONS)
		{
			*active = 1;
			*ly = 0x00;
		}
		else if(buttons & REMOTE_VIRTUAL_L2)
		{
			*active = 1;
			*lx = 0x00;
		}
		else if(buttons & REMOTE_VIRTUAL_R2)
		{
			*active = 1;
			*lx = 0xFF;
		}
	}
}

static void update_virtual_analog(unsigned int buttons)
{
	int active;
	unsigned char lx;
	unsigned char ly;
	int intc;

	calc_virtual_analog(buttons, &active, &lx, &ly);

	intc = pspSdkDisableInterrupts();
	g_virtual_analog_active = active;
	g_virtual_lx = lx;
	g_virtual_ly = ly;
	pspSdkEnableInterrupts(intc);
}

#if RJM_ENABLE_POPS_2P
static int has_virtual_analog_input(unsigned int buttons)
{
	int active;
	unsigned char lx;
	unsigned char ly;

	calc_virtual_analog(buttons, &active, &lx, &ly);
	return active;
}
#endif

static int special_button_thread(SceSize args, void *argp)
{
	unsigned int prev_buttons = 0;
	unsigned int seen_reset_serial = g_input_reset_serial;
	int vol_down_wait = 0;
	int vol_up_wait = 0;
	int sound_hold = 0;
	int sound_long_done = 0;
	int display_hold = 0;
	int display_long_done = 0;
	int prev_display_combo = 0;
#if RJM_ENABLE_POPS_2P && RJM_ENABLE_LOG
	int pops_diag_wait = 0;
	unsigned int pops_diag_prev_p2_hits = 0;
	unsigned int pops_diag_prev_gate_slot1 = 0;
	unsigned int pops_diag_prev_gate_slot2 = 0;
	unsigned int pops_diag_prev_gate_result[5] = { 0, 0, 0, 0, 0 };
#endif

	while(g_special_thread_run)
	{
		unsigned int buttons = get_current_buttons() & REMOTE_SPECIAL_BUTTONS;
		unsigned int pressed;
		int display_combo = ((buttons & PSP_CTRL_START) && (buttons & REMOTE_DISPLAY_COMBO_BUTTON));

		if(should_yield_to_pops_context())
		{
			cleanup_module("yield to POPS begin\n");
			sceKernelExitDeleteThread(0);
		}

		if(seen_reset_serial != g_input_reset_serial)
		{
			prev_buttons = buttons;
			seen_reset_serial = g_input_reset_serial;
			vol_down_wait = 0;
			vol_up_wait = 0;
			sound_hold = 0;
			sound_long_done = 0;
			display_hold = 0;
			display_long_done = 0;
			prev_display_combo = display_combo;
		}
		pressed = buttons & ~prev_buttons;

		update_virtual_analog(buttons);

#if RJM_ENABLE_POPS_2P
		if(is_pops_context())
		{
			if(g_p2_connected)
			{
				try_install_pops_gate_hook();
			}
			else
			{
				remove_pops_gate_hook();
			}

			if(g_pops_gate_state == 1)
			{
				update_pops_gate_mode();
#if RJM_ENABLE_LOG
				pops_diag_wait -= SPECIAL_POLL_MS;
				if(pops_diag_wait <= 0)
				{
					unsigned int p2_hits = g_pops_p2_hits;
					unsigned int gate_slot1 = g_pops_gate_slot1_hits;
					unsigned int gate_slot2 = g_pops_gate_slot2_hits;
					unsigned int gate_result[5];
					int j;

					for(j = 0; j < 5; j++)
					{
						gate_result[j] = g_pops_gate_result_counts[j];
					}

					rjmLogHex("pops p2 hits/s ", (int) (p2_hits - pops_diag_prev_p2_hits));
					rjmLogHex("pops last caller s0 ", (int) g_pops_caller_s0);
					rjmLogHex("pops gate mode ", g_pops_gate_mode);
					rjmLogHex("pops gate slot2 ret fixed ", RJM_POPS_GATE_SLOT2_RETURN);
					rjmLogHex("pops gate slot1/s ", (int) (gate_slot1 - pops_diag_prev_gate_slot1));
					rjmLogHex("pops gate slot2/s ", (int) (gate_slot2 - pops_diag_prev_gate_slot2));
					rjmLogHex("pops gate ret0/s ",
						(int) (gate_result[0] - pops_diag_prev_gate_result[0]));
					rjmLogHex("pops gate ret1/s ",
						(int) (gate_result[1] - pops_diag_prev_gate_result[1]));
					rjmLogHex("pops gate ret2/s ",
						(int) (gate_result[2] - pops_diag_prev_gate_result[2]));
					rjmLogHex("pops gate ret3/s ",
						(int) (gate_result[3] - pops_diag_prev_gate_result[3]));
					rjmLogHex("pops gate ret4/s ",
						(int) (gate_result[4] - pops_diag_prev_gate_result[4]));
					for(j = 0; j < 5; j++)
					{
						pops_diag_prev_gate_result[j] = gate_result[j];
					}
					pops_diag_prev_p2_hits = p2_hits;
					pops_diag_prev_gate_slot1 = gate_slot1;
					pops_diag_prev_gate_slot2 = gate_slot2;
					pops_diag_wait = 1000;

					rjmLogHex("pops p1 buttons ", (int) g_currjoy.Buttons);
					rjmLogHex("pops p2 buttons ", (int) g_p2joy.Buttons);
					rjmLogHex("pops p2 connected ", g_p2_connected);
				}
#endif
			}
		}
#endif

	if((pressed & REMOTE_HOME_BUTTON) && !is_pops_context())
	{
		handle_impose_buttons(PSP_CTRL_HOME);
	}
		if((buttons & PSP_CTRL_START) && (pressed & REMOTE_VIRTUAL_L2))
		{
			handle_impose_buttons(PSP_CTRL_VOLDOWN);
			vol_down_wait = VOLUME_REPEAT_DELAY_MS;
		}
		if((buttons & PSP_CTRL_START) && (pressed & REMOTE_VIRTUAL_R2))
		{
			handle_impose_buttons(PSP_CTRL_VOLUP);
			vol_up_wait = VOLUME_REPEAT_DELAY_MS;
		}
		if(pressed & REMOTE_SOUND_BUTTON)
		{
			sound_hold = 0;
			sound_long_done = 0;
		}
		if(display_combo && !prev_display_combo)
		{
			display_hold = 0;
			display_long_done = 0;
		}

		if((buttons & PSP_CTRL_START) && (buttons & REMOTE_VIRTUAL_L2))
		{
			vol_down_wait -= SPECIAL_POLL_MS;
			if(vol_down_wait <= 0)
			{
				handle_impose_buttons(PSP_CTRL_VOLDOWN);
				vol_down_wait = VOLUME_REPEAT_INTERVAL_MS;
			}
		}
		else
		{
			vol_down_wait = 0;
		}

		if((buttons & PSP_CTRL_START) && (buttons & REMOTE_VIRTUAL_R2))
		{
			vol_up_wait -= SPECIAL_POLL_MS;
			if(vol_up_wait <= 0)
			{
				handle_impose_buttons(PSP_CTRL_VOLUP);
				vol_up_wait = VOLUME_REPEAT_INTERVAL_MS;
			}
		}
		else
		{
			vol_up_wait = 0;
		}

		if(buttons & REMOTE_SOUND_BUTTON)
		{
			sound_hold += SPECIAL_POLL_MS;
			if(!sound_long_done && sound_hold >= SOUND_HOLD_MS)
			{
				handle_sound_hold();
				sound_long_done = 1;
			}
		}
		if((prev_buttons & REMOTE_SOUND_BUTTON) && ((buttons & REMOTE_SOUND_BUTTON) == 0))
		{
			if(!sound_long_done)
			{
				handle_impose_buttons(PSP_CTRL_NOTE);
			}
			sound_hold = 0;
			sound_long_done = 0;
		}

		if(display_combo)
		{
			display_hold += SPECIAL_POLL_MS;
			if(!display_long_done && display_hold >= DISPLAY_HOLD_MS)
			{
				rjmLogText("display hold ignored: tvout needs VSH display switch\n");
				display_long_done = 1;
			}
		}
		if(prev_display_combo && !display_combo)
		{
			if(!display_long_done)
			{
				cycle_impose_param(PSP_IMPOSE_BACKLIGHT_BRIGHTNESS,
					PSP_BRIGHTNESS_MIN, PSP_BRIGHTNESS_MAX);
			}
			display_hold = 0;
			display_long_done = 0;
		}

		prev_display_combo = display_combo;
		prev_buttons = buttons;
		sceKernelDelayThread(SPECIAL_POLL_US);
	}

	return 0;
}

static void update_system_button_intercepts(unsigned int buttons)
{
	unsigned int pressed = 0;
	unsigned int changed = pressed ^ g_intercept_buttons;
	unsigned int k1;

	if(!is_pops_context() && (buttons & REMOTE_HOME_BUTTON))
	{
		pressed |= REMOTE_HOME_BUTTON;
	}
	changed = pressed ^ g_intercept_buttons;
	if(changed == 0)
	{
		return;
	}

	k1 = psplinkSetK1(0);
	if(changed & pressed)
	{
		sceCtrlSetButtonIntercept(changed & pressed, SCE_CTRL_MASK_APPLY_BUTTONS);
	}
	if(changed & g_intercept_buttons)
	{
		sceCtrlSetButtonIntercept(changed & g_intercept_buttons, SCE_CTRL_MASK_NO_MASK);
	}
	psplinkSetK1(k1);

	g_intercept_buttons = pressed;
}

static void inject_remote_values(SceCtrlData *pad_data, int count, int neg)
{
	int i;
	int intc;
	unsigned int raw_buttons;
	unsigned int buttons;
	unsigned char lx;
	unsigned char ly;
	int virtual_active;
	int k1;

	intc = pspSdkDisableInterrupts();
	raw_buttons = g_currjoy.Buttons;
	buttons = sanitize_remote_buttons(raw_buttons);
	asm __volatile__ ( "move %0, $k1" : "=r"(k1) );
	if(k1)
	{
		buttons &= ~calc_mask();
	}
	calc_virtual_analog(raw_buttons, &virtual_active, &lx, &ly);
	if(virtual_active)
	{
		g_virtual_analog_active = 1;
		g_virtual_lx = lx;
		g_virtual_ly = ly;
	}
	else
	{
		g_virtual_analog_active = 0;
		lx = g_currjoy.Lx;
		ly = g_currjoy.Ly;
	}

	for(i = 0; i < count; i++)
	{
		if(neg)
		{
			pad_data[i].Buttons &= ~buttons;
		}
		else
		{
			pad_data[i].Buttons |= buttons;
		}

		pad_data[i].Lx = map_axis(pad_data[i].Lx, lx);
		pad_data[i].Ly = map_axis(pad_data[i].Ly, ly);
	}

	pspSdkEnableInterrupts(intc);
}

static int ctrl_hook_func(SceCtrlData *pad_data, int count, int type)
{
	int ret = g_ctrl_common(pad_data, count, type);

	if(ret > 0)
	{
		inject_remote_values(pad_data, ret, type & 1);
	}

	return ret;
}

static int ctrl_peek_positive_hook(SceCtrlData *pad_data, int count)
{
	int ret = g_ctrl_peek_positive_common(pad_data, count);

	if(ret > 0)
	{
#if RJM_ENABLE_LOG
		if(get_remote_inject_buttons())
		{
			rjmLogHex("buffer peek pos ", ret);
		}
#endif
		inject_remote_values(pad_data, ret, 0);
	}

	return ret;
}

#if RJM_ENABLE_POPS_2P
/* External linkage: this is the jump target patched into the real
 * sceCtrlPeekBufferNegative via pops_ctrl_peek_negative_entry (pops_hook.S),
 * which must be able to resolve its symbol at link time. */
int ctrl_peek_negative_hook(SceCtrlData *pad_data, int count)
#else
static int ctrl_peek_negative_hook(SceCtrlData *pad_data, int count)
#endif
{
	int ret;

#if RJM_ENABLE_POPS_2P
	if(g_pops_gate_state == 1 && g_pops_caller_s0 == g_pops_p2_marker)
	{
		int i;
		int intc;
		int hwret;
		int virtual_active;
		SceCtrlData remote;
		unsigned char virtual_lx;
		unsigned char virtual_ly;

		intc = pspSdkDisableInterrupts();
		remote = g_p2joy;
		pspSdkEnableInterrupts(intc);

		calc_virtual_analog(remote.Buttons, &virtual_active,
			&virtual_lx, &virtual_ly);

		/*
		 * Slot 2 has its own state stream from the RP2040 multitap reader.
		 * Keep applying the normal sanitize step so internal/combo buttons
		 * never leak into POPS port B as plain game input.
		 */
		remote.Buttons = sanitize_remote_buttons(remote.Buttons);

		hwret = g_ctrl_peek_negative_common(pad_data, count);
		if(hwret <= 0)
		{
			return hwret;
		}

		/*
		 * Keep port-B analog centered except for the explicit L2/R2 virtual
		 * analog mapping. L2/R2 are intentionally carried through this analog
		 * path to match the single-controller POPS behavior and POPS's own
		 * controller assignment.
		 */
		for(i = 0; i < hwret; i++)
		{
			unsigned char lx = virtual_active ? virtual_lx : ANALOG_CENTER;
			unsigned char ly = virtual_active ? virtual_ly : ANALOG_CENTER;

			/*
			 * This hook replaces sceCtrlPeekBufferNegative(), whose button
			 * bits are active-low: a set bit means "not pressed", and a
			 * cleared bit means "pressed". g_currjoy is stored in the usual
			 * positive form, so port B must first be reset to all released
			 * for the game-relevant buttons, then clear only the requested
			 * remote buttons. Returning positive-form bits here makes idle
			 * input look like held buttons to POPS, which manifests as
			 * phantom movement and movie/menu skips.
			 */
			pad_data[i].Buttons = (pad_data[i].Buttons | POPS_P2_GAME_BUTTONS_MASK) &
				~(remote.Buttons & POPS_P2_GAME_BUTTONS_MASK);
			pad_data[i].Lx = lx;
			pad_data[i].Ly = ly;
		}
		g_pops_p2_hits++;

		return hwret;
	}
#endif

	ret = g_ctrl_peek_negative_common(pad_data, count);

	if(ret > 0)
	{
#if RJM_ENABLE_LOG
		if(get_remote_inject_buttons())
		{
			rjmLogHex("buffer peek neg ", ret);
		}
#endif
		inject_remote_values(pad_data, ret, 1);
	}

	return ret;
}

static int ctrl_read_positive_hook(SceCtrlData *pad_data, int count)
{
	int ret = g_ctrl_read_positive_common(pad_data, count);

	if(ret > 0)
	{
#if RJM_ENABLE_LOG
		if(get_remote_inject_buttons())
		{
			rjmLogHex("buffer read pos ", ret);
		}
#endif
		inject_remote_values(pad_data, ret, 0);
	}

	return ret;
}

static int ctrl_read_negative_hook(SceCtrlData *pad_data, int count)
{
	int ret = g_ctrl_read_negative_common(pad_data, count);

	if(ret > 0)
	{
#if RJM_ENABLE_LOG
		if(get_remote_inject_buttons())
		{
			rjmLogHex("buffer read neg ", ret);
		}
#endif
		inject_remote_values(pad_data, ret, 1);
	}

	return ret;
}

static int ctrl_peek_latch_hook(SceCtrlLatch *latch)
{
	int ret = g_ctrl_peek_latch_common(latch);
	unsigned int buttons;
	int intc;

	if(ret < 0)
	{
		return ret;
	}

	intc = pspSdkDisableInterrupts();
	buttons = get_remote_inject_buttons();
	latch->uiMake |= g_remote_latch_make;
	latch->uiBreak |= g_remote_latch_break;
	latch->uiPress |= buttons;
	latch->uiRelease &= ~buttons;
#if RJM_ENABLE_LOG
	if(buttons || g_remote_latch_make || g_remote_latch_break)
	{
		rjmLogHex("peek latch buttons ", buttons);
	}
#endif
	pspSdkEnableInterrupts(intc);

	return ret;
}

static int ctrl_read_latch_hook(SceCtrlLatch *latch)
{
	int ret = g_ctrl_read_latch_common(latch);
	unsigned int buttons;
	int intc;

	if(ret < 0)
	{
		return ret;
	}

	intc = pspSdkDisableInterrupts();
	buttons = get_remote_inject_buttons();
	latch->uiMake |= g_remote_latch_make;
	latch->uiBreak |= g_remote_latch_break;
	latch->uiPress |= buttons;
	latch->uiRelease &= ~buttons;
#if RJM_ENABLE_LOG
	if(buttons || g_remote_latch_make || g_remote_latch_break)
	{
		rjmLogHex("read latch buttons ", buttons);
	}
#endif
	g_remote_latch_make = 0;
	g_remote_latch_break = 0;
	pspSdkEnableInterrupts(intc);

	return ret;
}

static void patch_function_jump(unsigned int target, void *hook, void **original,
	unsigned int *trampoline)
{
	if(target == 0 || original == NULL || *original != NULL)
	{
		return;
	}

	trampoline[0] = _lw(target);
	trampoline[1] = _lw(target + 4);
	trampoline[2] = 0;
	trampoline[3] = 0x08000000 | (((target + 8) & 0x0FFFFFFC) >> 2);
	trampoline[4] = 0;

	_sw(0x08000000 | ((((unsigned int) hook) & 0x0FFFFFFC) >> 2), target);
	_sw(0, target + 4);
	sceKernelDcacheWritebackInvalidateRange((void *) target, 8);
	sceKernelIcacheInvalidateRange((void *) target, 8);
	sceKernelDcacheWritebackInvalidateRange(trampoline, 20);
	sceKernelIcacheInvalidateRange(trampoline, 20);
	*original = trampoline;
}

static void restore_function_jump(unsigned int target, void **original,
	unsigned int *trampoline)
{
	if(target == 0 || original == NULL || *original == NULL)
	{
		return;
	}

	_sw(trampoline[0], target);
	_sw(trampoline[1], target + 4);
	sceKernelDcacheWritebackInvalidateRange((void *) target, 8);
	sceKernelIcacheInvalidateRange((void *) target, 8);
	*original = NULL;
}

static void restore_ctrl_hooks(void)
{
	restore_function_jump(g_ctrl_peek_positive_target,
		(void **) &g_ctrl_peek_positive_common, g_ctrl_peek_positive_trampoline);
	restore_function_jump(g_ctrl_peek_negative_target,
		(void **) &g_ctrl_peek_negative_common, g_ctrl_peek_negative_trampoline);
	restore_function_jump(g_ctrl_read_positive_target,
		(void **) &g_ctrl_read_positive_common, g_ctrl_read_positive_trampoline);
	restore_function_jump(g_ctrl_read_negative_target,
		(void **) &g_ctrl_read_negative_common, g_ctrl_read_negative_trampoline);
	restore_function_jump(g_ctrl_peek_latch_target,
		(void **) &g_ctrl_peek_latch_common, g_ctrl_peek_latch_trampoline);
	restore_function_jump(g_ctrl_read_latch_target,
		(void **) &g_ctrl_read_latch_common, g_ctrl_read_latch_trampoline);
}

static void hook_ctrl_latch_functions(void)
{
	g_ctrl_peek_latch_target = GET_JUMP_TARGET(_lw((unsigned int) sceCtrlPeekLatch));
	g_ctrl_read_latch_target = GET_JUMP_TARGET(_lw((unsigned int) sceCtrlReadLatch));

	rjmLogHex("ctrl peek latch target ", g_ctrl_peek_latch_target);
	rjmLogHex("ctrl read latch target ", g_ctrl_read_latch_target);
	patch_function_jump(g_ctrl_peek_latch_target, ctrl_peek_latch_hook,
		(void **) &g_ctrl_peek_latch_common, g_ctrl_peek_latch_trampoline);
	patch_function_jump(g_ctrl_read_latch_target, ctrl_read_latch_hook,
		(void **) &g_ctrl_read_latch_common, g_ctrl_read_latch_trampoline);
}

static void hook_ctrl_buffer_entry_functions(void)
{
	g_ctrl_peek_positive_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlPeekBufferPositive));
	g_ctrl_peek_negative_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlPeekBufferNegative));
	g_ctrl_read_positive_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlReadBufferPositive));
	g_ctrl_read_negative_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlReadBufferNegative));

	rjmLogHex("ctrl peek pos target ", g_ctrl_peek_positive_target);
	rjmLogHex("ctrl peek neg target ", g_ctrl_peek_negative_target);
	rjmLogHex("ctrl read pos target ", g_ctrl_read_positive_target);
	rjmLogHex("ctrl read neg target ", g_ctrl_read_negative_target);
	patch_function_jump(g_ctrl_peek_positive_target, ctrl_peek_positive_hook,
		(void **) &g_ctrl_peek_positive_common, g_ctrl_peek_positive_trampoline);
#if RJM_ENABLE_POPS_2P
	patch_function_jump(g_ctrl_peek_negative_target, pops_ctrl_peek_negative_entry,
		(void **) &g_ctrl_peek_negative_common, g_ctrl_peek_negative_trampoline);
#else
	patch_function_jump(g_ctrl_peek_negative_target, ctrl_peek_negative_hook,
		(void **) &g_ctrl_peek_negative_common, g_ctrl_peek_negative_trampoline);
#endif
	patch_function_jump(g_ctrl_read_positive_target, ctrl_read_positive_hook,
		(void **) &g_ctrl_read_positive_common, g_ctrl_read_positive_trampoline);
	patch_function_jump(g_ctrl_read_negative_target, ctrl_read_negative_hook,
		(void **) &g_ctrl_read_negative_common, g_ctrl_read_negative_trampoline);
}

#if RJM_ENABLE_POPS_2P
#define POPS_GATE_DUMP_WORD_COUNT 24
static void patch_pops_gate_fast_return4(void)
{
	if(g_pops_gate_mode == 1)
	{
		return;
	}

	_sw(0x03E00008u, g_pops_gate_target);
	_sw(0x24020004u, g_pops_gate_target + 4);
	sceKernelDcacheWritebackInvalidateRange((void *) g_pops_gate_target, 8);
	sceKernelIcacheInvalidateRange((void *) g_pops_gate_target, 8);
	g_pops_gate_mode = 1;
}

static void patch_pops_gate_slot2_asm(void)
{
	if(g_pops_gate_mode == 2)
	{
		return;
	}

	_sw(0x08000000 | ((((unsigned int) pops_gate_entry) & 0x0FFFFFFC) >> 2),
		g_pops_gate_target);
	_sw(0, g_pops_gate_target + 4);
	sceKernelDcacheWritebackInvalidateRange((void *) g_pops_gate_target, 8);
	sceKernelIcacheInvalidateRange((void *) g_pops_gate_target, 8);
	g_pops_gate_mode = 2;
}

static void update_pops_gate_mode(void)
{
	unsigned int p1_buttons;
	unsigned int p2_buttons;
	int intc;
	int virtual_active;

	if(g_pops_gate_state != 1)
	{
		return;
	}

#if RJM_POPS_GATE_FORCE_SLOT2_ASM
	patch_pops_gate_slot2_asm();
	return;
#endif

	intc = pspSdkDisableInterrupts();
	p1_buttons = g_currjoy.Buttons;
	p2_buttons = g_p2joy.Buttons;
	pspSdkEnableInterrupts(intc);

	virtual_active = has_virtual_analog_input(p1_buttons) ||
		has_virtual_analog_input(p2_buttons);
	if(virtual_active && !g_pops_gate_virtual_active)
	{
		g_pops_gate_slot2_pulse_ms = POPS_GATE_SLOT2_PULSE_MS;
	}
	else if(!virtual_active)
	{
		g_pops_gate_slot2_pulse_ms = 0;
	}
	g_pops_gate_virtual_active = virtual_active;

	if(g_pops_gate_slot2_pulse_ms > 0)
	{
		patch_pops_gate_slot2_asm();
		g_pops_gate_slot2_pulse_ms -= SPECIAL_POLL_MS;
	}
	else
	{
		patch_pops_gate_fast_return4();
	}
}

#if RJM_ENABLE_LOG
static void dump_pops_gate_code(unsigned int target)
{
	int i;

	rjmLogText("pops gate code dump begin\n");
	for(i = 0; i < POPS_GATE_DUMP_WORD_COUNT; i++)
	{
		rjmLogHex("pops gate word ", _lw(target + i * 4));
	}
	rjmLogText("pops gate code dump end\n");
}
#else
static void dump_pops_gate_code(unsigned int target)
{
	(void) target;
}
#endif

/*
 * pops_XXg.prx is loaded dynamically by scePopsMan well after our own
 * module_start, and its load base varies per boot, so this is polled from
 * special_button_thread until it succeeds (or permanently gives up).
 * FUN_0002c718's machine code is identical across every POPS generation
 * sampled (01g-11g); only its offset from the module base differs, so each
 * known offset is tried and verified by signature before patching.
 *
 * Also computes g_pops_p2_marker here: FUN_0000a250 keeps the slot's
 * struct-base pointer (gp + 0x3c00 + slot*0x30) in $s0 for its entire body,
 * confirmed identical across all 7 sampled generations - this is the value
 * the sceCtrlPeekBufferNegative hook looks for via pops_hook.S's captured
 * $s0 to recognize a port-B call. This is a read-only computation, no write
 * into pops_XXg.prx happens for this part.
 */
static void try_install_pops_gate_hook(void)
{
	SceModule *mod;
	unsigned int base;
	unsigned int target = 0;
	int i;

	if(g_pops_gate_state != 0)
	{
		return;
	}

	mod = sceKernelFindModuleByName(POPS_MODULE_NAME);
	if(mod == NULL)
	{
		return;
	}

	base = mod->text_addr;
	/*
	 * NOT base + gp_value here: real-hardware logging showed $s0 at the
	 * sceCtrlPeekBufferNegative call site is gp_value + 0x3c00 + slot*0x30
	 * directly (e.g. 0x13c30 for slot 1, matching gp_value=0x10000 with no
	 * base added at all). pops_XXg.prx's $gp does not point into its own
	 * relocated data segment - it points at PSP scratchpad RAM, a small
	 * fixed-address on-chip region PSP-wiki documents POPS using for
	 * frequently-touched emulation state (GTE registers at 0x10000, PS1
	 * scratchpad at 0x13000), so it never needs relocation regardless of
	 * where the module's own code loads.
	 */
	g_pops_p2_marker = mod->gp_value + POPS_PAD_SLOT_BASE +
		POPS_PAD_SLOT_P2 * POPS_PAD_SLOT_STRIDE;

	rjmLogHex("pops mod text_addr ", (int) base);
	rjmLogHex("pops p2 marker ", (int) g_pops_p2_marker);

	for(i = 0; i < POPS_GATE_OFFSET_COUNT; i++)
	{
		unsigned int candidate = base + g_pops_gate_offsets[i];

		if(_lw(candidate + POPS_GATE_SIG_OFFSET) == POPS_GATE_SIG0 &&
			_lw(candidate + POPS_GATE_SIG_OFFSET + 4) == POPS_GATE_SIG1 &&
			_lw(candidate + POPS_GATE_SIG_OFFSET + 8) == POPS_GATE_SIG2)
		{
			target = candidate;
			break;
		}
	}

	if(target == 0)
	{
		rjmLogText("pops gate hook: signature not found\n");
		g_pops_gate_state = -1;
		return;
	}

	rjmLogHex("pops gate hook signature matched at ", target);
	dump_pops_gate_code(target);
	if((_lw(target) & 0xFFFF0000u) != 0x3C020000u ||
		(_lw(target + 4) & 0xFFFF0000u) != 0x24430000u)
	{
		rjmLogText("pops gate hook: data-base signature not found\n");
		g_pops_gate_state = -1;
		return;
	}
	g_pops_gate_data_base = ((_lw(target) & 0xFFFFu) << 16) +
		(short)(_lw(target + 4) & 0xFFFFu);
	rjmLogHex("pops gate data base ", (int) g_pops_gate_data_base);

	g_pops_gate_target = target;
	g_pops_gate_trampoline[0] = _lw(target);
	g_pops_gate_trampoline[1] = _lw(target + 4);
	g_pops_gate_trampoline[2] = 0;
	g_pops_gate_trampoline[3] = 0x08000000 | (((target + 8) & 0x0FFFFFFC) >> 2);
	g_pops_gate_trampoline[4] = 0;
	sceKernelDcacheWritebackInvalidateRange(g_pops_gate_trampoline, 20);
	sceKernelIcacheInvalidateRange(g_pops_gate_trampoline, 20);
	g_pops_gate_state = 1;
	g_pops_gate_mode = 0;
	g_pops_gate_virtual_active = 0;
	g_pops_gate_slot2_pulse_ms = 0;
	update_pops_gate_mode();
	rjmLogHex("pops gate hook installed at ", target);
}

/*
 * Undoes the patch above, if installed. Without this, unloading (and later
 * reloading, e.g. across the VSH->POPS reboot-load transition) leaves
 * pops_XXg.prx jumping into memory that belonged to this now-unloaded PRX
 * instance - confirmed to cause real problems for the earlier FUN_0000a250
 * hook, so applying the same fix here from the start.
 */
static void remove_pops_gate_hook(void)
{
	if(g_pops_gate_state != 1)
	{
		return;
	}

	_sw(g_pops_gate_trampoline[0], g_pops_gate_target);
	_sw(g_pops_gate_trampoline[1], g_pops_gate_target + 4);
	sceKernelDcacheWritebackInvalidateRange((void *) g_pops_gate_target, 8);
	sceKernelIcacheInvalidateRange((void *) g_pops_gate_target, 8);

	g_pops_gate_state = 0;
	g_pops_gate_mode = 0;
	g_pops_gate_virtual_active = 0;
	g_pops_gate_slot2_pulse_ms = 0;
	rjmLogHex("pops gate hook removed at ", g_pops_gate_target);
}
#endif

static int hook_ctrl_function(unsigned int *jump) __attribute__((unused));
static int hook_ctrl_function(unsigned int *jump)
{
	unsigned int target = GET_JUMP_TARGET(*jump);
	unsigned int patch_addr = 0;
	int inst = 0;
	int i;

	for(i = 0; i < 8; i++)
	{
		unsigned int addr = target + (i * 4);
		inst = _lw(addr);
		if((inst & ~0x03FFFFFF) == 0x0C000000)
		{
			patch_addr = addr;
			break;
		}
	}

	if(patch_addr == 0)
	{
		return 1;
	}

	g_ctrl_common = (void *) GET_JUMP_TARGET(inst);
	_sw(0x0C000000 | (((unsigned int) ctrl_hook_func & 0x0FFFFFFF) >> 2),
		patch_addr);

	return 0;
}

static void apply_joyevent_to_pad(SceCtrlData *pad, const struct JoyEvent *joyevent,
	int button_down_type, int button_up_type, int analog_x_type, int analog_y_type)
{
	unsigned int new_buttons = pad->Buttons;

	if(joyevent->type == button_up_type)
	{
		new_buttons &= ~joyevent->value;
	}
	else if(joyevent->type == button_down_type)
	{
		new_buttons |= joyevent->value;
	}
	else if(joyevent->type == analog_y_type)
	{
		pad->Ly = joyevent->value;
	}
	else if(joyevent->type == analog_x_type)
	{
		pad->Lx = joyevent->value;
	}

	pad->Buttons = new_buttons;
}

static void handle_joyevent(const struct JoyEvent *joyevent)
{
	int intc = pspSdkDisableInterrupts();
	unsigned int old_buttons = g_currjoy.Buttons;
	unsigned int new_buttons;
	unsigned int old_inject;
	unsigned int new_inject;

	switch(joyevent->type)
	{
		case TYPE_BUTTON_UP:
		case TYPE_BUTTON_DOWN:
		case TYPE_ANALOG_Y:
		case TYPE_ANALOG_X:
			apply_joyevent_to_pad(&g_currjoy, joyevent,
				TYPE_BUTTON_DOWN, TYPE_BUTTON_UP, TYPE_ANALOG_X, TYPE_ANALOG_Y);
			break;
#if RJM_ENABLE_POPS_2P
		case TYPE_P2_BUTTON_UP:
		case TYPE_P2_BUTTON_DOWN:
		case TYPE_P2_ANALOG_Y:
		case TYPE_P2_ANALOG_X:
			apply_joyevent_to_pad(&g_p2joy, joyevent,
				TYPE_P2_BUTTON_DOWN, TYPE_P2_BUTTON_UP,
				TYPE_P2_ANALOG_X, TYPE_P2_ANALOG_Y);
			break;
		case TYPE_P2_STATUS:
			g_p2_connected = joyevent->value != 0;
			if(!g_p2_connected)
			{
				g_p2joy.Buttons = 0;
				g_p2joy.Lx = ANALOG_CENTER;
				g_p2joy.Ly = ANALOG_CENTER;
			}
			break;
#endif
		default:
			break;
	}
	new_buttons = g_currjoy.Buttons;

	old_inject = sanitize_remote_buttons(old_buttons);
	new_inject = sanitize_remote_buttons(new_buttons);
	g_remote_latch_make |= (~old_inject & new_inject);
	g_remote_latch_break |= (old_inject & ~new_inject);
	if(joyevent->type == TYPE_BUTTON_UP || joyevent->type == TYPE_BUTTON_DOWN)
	{
		rjmLogHex("joy buttons ", new_buttons);
	}
#if RJM_ENABLE_POPS_2P
	else if(joyevent->type == TYPE_P2_BUTTON_UP || joyevent->type == TYPE_P2_BUTTON_DOWN)
	{
		rjmLogHex("joy p2 buttons ", g_p2joy.Buttons);
	}
	else if(joyevent->type == TYPE_P2_STATUS)
	{
		rjmLogHex("joy p2 connected ", g_p2_connected);
	}
#endif
	else if(joyevent->type == TYPE_ANALOG_X || joyevent->type == TYPE_ANALOG_Y ||
		joyevent->type == TYPE_P2_ANALOG_X || joyevent->type == TYPE_P2_ANALOG_Y)
	{
		rjmLogHex("joy analog type ", joyevent->type);
		rjmLogHex("joy analog value ", joyevent->value);
	}

	pspSdkEnableInterrupts(intc);
#if RJM_ENABLE_POPS_2P
	if(is_pops_context() && g_pops_gate_state == 1)
	{
		update_pops_gate_mode();
	}
#endif
	update_system_button_intercepts(g_currjoy.Buttons);
}

static int main_thread(SceSize args, void *argp)
{
	rjmLogText("main_thread begin\n");

	sceKernelDcacheWritebackInvalidateAll();
	sceKernelIcacheInvalidateAll();

	load_usb_module();
	sceKernelDelayThread(1000000 * 3);
	rjmLogText("register usb\n");
	rjmUsbRegister();
	rjmLogText("start usb\n");
	if(rjmUsbStart() != 0)
	{
		rjmLogText("start usb failed\n");
		rjmUsbShutdown();
		sceKernelExitDeleteThread(0);
	}
	rjmLogText("start usb ok\n");

	hook_ctrl_buffer_entry_functions();
	rjmLogText("ctrl hook ok\n");
	hook_ctrl_latch_functions();

	rjmLogText("usb suspend/resume\n");
	rjmUsbSuspend();
	rjmUsbResume();
	rjmUsbAsyncFlush();

	while(1)
	{
		struct JoyEvent joyevent;
		int len;

		reset_remote_input_state();
		rjmUsbAsyncFlush();
		rjmLogText("waiting connect\n");
		rjmUsbWaitForConnect();
		rjmLogText("connected\n");

		while(rjmUsbIsConnected())
		{
			len = rjmUsbAsyncRead((void *) &joyevent, sizeof(joyevent));
			if((len != sizeof(joyevent)) || (joyevent.magic != JOY_MAGIC))
			{
				reset_remote_input_state();
				if(len >= 0)
				{
					rjmUsbAsyncFlush();
				}
				else
				{
					break;
				}
				continue;
			}

			handle_joyevent(&joyevent);
			scePowerTick(0);
		}
	}

	return 0;
}

static int power_callback(int unknown, int power_info, void *common)
{
	(void) unknown;
	(void) common;

	if(power_info & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY))
	{
		rjmLogHex("power suspend flags ", power_info);
		reset_remote_input_state();
		rjmUsbSuspend();
		rjmUsbAsyncFlush();
	}
	if(power_info & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE))
	{
		rjmLogHex("power resume flags ", power_info);
		rjmUsbResume();
		rjmUsbAsyncFlush();
	}

	return 0;
}

static int power_thread(SceSize args, void *argp)
{
	(void) args;
	(void) argp;

	g_power_cbid = sceKernelCreateCallback("RJMStandalonePower", power_callback, NULL);
	if(g_power_cbid >= 0)
	{
		g_power_slot = scePowerRegisterCallback(-1, g_power_cbid);
		rjmLogHex("power_slot ", g_power_slot);
	}
	sceKernelSleepThreadCB();

	return 0;
}

int module_start(SceSize args, void *argp)
{
	rjmLogInit();
	rjmLogText("module_start\n");
	rjmLogHex("init apitype ", sceKernelInitApitype());
	rjmLogHex("init keyconfig ", sceKernelApplicationType());
	rjmLogHex("module scePops_Manager ", (int) sceKernelFindModuleByName("scePops_Manager"));
	rjmLogHex("module pops ", (int) sceKernelFindModuleByName("pops"));
	rjmLogHex("module popsman ", (int) sceKernelFindModuleByName("popsman"));
	g_is_pops_context = sceKernelApplicationType() == PSP_INIT_KEYCONFIG_POPS ||
		sceKernelFindModuleByName("scePops_Manager") != NULL;
	rjmUsbSetPopsContext(g_is_pops_context);
	reserve_pops_reboot_load();
	memset(&g_currjoy, 0, sizeof(g_currjoy));
	g_currjoy.Lx = ANALOG_CENTER;
	g_currjoy.Ly = ANALOG_CENTER;
	memset(&g_p2joy, 0, sizeof(g_p2joy));
	g_p2joy.Lx = ANALOG_CENTER;
	g_p2joy.Ly = ANALOG_CENTER;
	g_p2_connected = 0;
	g_special_thread_run = 1;
	g_virtual_lx = ANALOG_CENTER;
	g_virtual_ly = ANALOG_CENTER;

	g_special_thid = sceKernelCreateThread("RJMStandaloneSpecial",
		special_button_thread, 16, 0x800, 0, NULL);
	if(g_special_thid >= 0)
	{
		sceKernelStartThread(g_special_thid, args, argp);
	}
	rjmLogHex("special_thid ", g_special_thid);

	g_power_thid = sceKernelCreateThread("RJMStandalonePower",
		power_thread, 16, 0x800, 0, NULL);
	if(g_power_thid >= 0)
	{
		sceKernelStartThread(g_power_thid, args, argp);
	}
	rjmLogHex("power_thid ", g_power_thid);

	g_main_thid = sceKernelCreateThread("RJMStandalone",
		main_thread, 15, 0x1000, 0, NULL);
	if(g_main_thid >= 0)
	{
		sceKernelStartThread(g_main_thid, args, argp);
	}
	rjmLogHex("main_thid ", g_main_thid);

	return 0;
}

static void cleanup_module(const char *reason)
{
	SceUID self;

	if(g_cleanup_started)
	{
		rjmLogText("cleanup already started\n");
		return;
	}
	g_cleanup_started = 1;
	self = sceKernelGetThreadId();

	rjmLogText(reason);
	rjmLogHex("cleanup apitype ", sceKernelInitApitype());
	rjmLogHex("cleanup keyconfig ", sceKernelApplicationType());
	rjmLogHex("cleanup scePops_Manager ", (int) sceKernelFindModuleByName("scePops_Manager"));
	g_special_thread_run = 0;
	rjmLogText("cleanup reset input\n");
	reset_remote_input_state();

#if RJM_ENABLE_POPS_2P
	remove_pops_gate_hook();
#endif
	restore_ctrl_hooks();
	rjmLogText("cleanup ctrl hooks restored\n");

	if(g_intercept_buttons)
	{
		unsigned int k1 = psplinkSetK1(0);
		sceCtrlSetButtonIntercept(g_intercept_buttons, SCE_CTRL_MASK_NO_MASK);
		psplinkSetK1(k1);
		g_intercept_buttons = 0;
	}
	rjmLogText("cleanup intercept cleared\n");

	if(g_main_thid >= 0 && g_main_thid != self)
	{
		rjmLogHex("cleanup delete main ", g_main_thid);
		sceKernelTerminateDeleteThread(g_main_thid);
		g_main_thid = -1;
	}
	if(g_special_thid >= 0 && g_special_thid != self)
	{
		rjmLogHex("cleanup delete special ", g_special_thid);
		sceKernelTerminateDeleteThread(g_special_thid);
		g_special_thid = -1;
	}
	if(g_power_slot >= 0)
	{
		rjmLogHex("cleanup unregister power ", g_power_slot);
		scePowerUnregisterCallback(g_power_slot);
		g_power_slot = -1;
	}
	if(g_power_cbid >= 0)
	{
		rjmLogHex("cleanup delete power cb ", g_power_cbid);
		sceKernelDeleteCallback(g_power_cbid);
		g_power_cbid = -1;
	}
	if(g_power_thid >= 0 && g_power_thid != self)
	{
		rjmLogHex("cleanup delete power ", g_power_thid);
		sceKernelTerminateDeleteThread(g_power_thid);
		g_power_thid = -1;
	}

	rjmLogText("cleanup usb shutdown\n");
	rjmUsbShutdown();
	rjmLogText("cleanup done\n");
}

int module_stop(SceSize args, void *argp)
{
	cleanup_module("module_stop begin\n");

	return 0;
}

int module_reboot_before(SceSize args, void *argp)
{
	cleanup_module("module_reboot_before begin\n");

	return 0;
}

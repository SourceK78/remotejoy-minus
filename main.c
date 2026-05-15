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

static SceCtrlData g_currjoy;
static unsigned int g_remote_latch_make;
static unsigned int g_remote_latch_break;
static int (*g_ctrl_common)(SceCtrlData *, int count, int type);
static int (*g_ctrl_peek_positive_common)(SceCtrlData *, int count);
static int (*g_ctrl_peek_negative_common)(SceCtrlData *, int count);
static int (*g_ctrl_read_positive_common)(SceCtrlData *, int count);
static int (*g_ctrl_read_negative_common)(SceCtrlData *, int count);
static int (*g_ctrl_peek_latch_common)(SceCtrlLatch *);
static int (*g_ctrl_read_latch_common)(SceCtrlLatch *);
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

unsigned int psplinkSetK1(unsigned int k1);
void sceImposeHomeButton(int open);
int sceImposeGetStatus(void);

static void update_system_button_intercepts(unsigned int buttons);
static void reset_remote_input_state(void);
static unsigned int calc_mask(void);

static int is_pops_context(void)
{
	return g_is_pops_context;
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
	g_remote_latch_make = 0;
	g_remote_latch_break = 0;
	g_virtual_analog_active = 0;
	g_virtual_lx = ANALOG_CENTER;
	g_virtual_ly = ANALOG_CENTER;
	g_input_reset_serial++;
	pspSdkEnableInterrupts(intc);

	update_system_button_intercepts(0);
}

static void update_virtual_analog(unsigned int buttons)
{
	int active = 0;
	unsigned char lx = ANALOG_CENTER;
	unsigned char ly = ANALOG_CENTER;
	int intc;

	if((buttons & PSP_CTRL_START) == 0)
	{
		if((buttons & REMOTE_VOLUME_COMBO_BUTTONS) == REMOTE_VOLUME_COMBO_BUTTONS)
		{
			active = 1;
			ly = 0x00;
		}
		else if(buttons & REMOTE_VIRTUAL_L2)
		{
			active = 1;
			lx = 0x00;
		}
		else if(buttons & REMOTE_VIRTUAL_R2)
		{
			active = 1;
			lx = 0xFF;
		}
	}

	intc = pspSdkDisableInterrupts();
	g_virtual_analog_active = active;
	g_virtual_lx = lx;
	g_virtual_ly = ly;
	pspSdkEnableInterrupts(intc);
}

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

	while(g_special_thread_run)
	{
		unsigned int buttons = get_current_buttons() & REMOTE_SPECIAL_BUTTONS;
		unsigned int pressed;
		int display_combo = ((buttons & PSP_CTRL_START) && (buttons & REMOTE_DISPLAY_COMBO_BUTTON));

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
	unsigned int buttons;
	unsigned char lx;
	unsigned char ly;

	intc = pspSdkDisableInterrupts();
	buttons = get_remote_inject_buttons();
	if(g_virtual_analog_active)
	{
		lx = g_virtual_lx;
		ly = g_virtual_ly;
	}
	else
	{
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

static int ctrl_peek_negative_hook(SceCtrlData *pad_data, int count)
{
	int ret = g_ctrl_peek_negative_common(pad_data, count);

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

static void hook_ctrl_latch_functions(void)
{
	static unsigned int peek_trampoline[5] __attribute__((aligned(16)));
	static unsigned int read_trampoline[5] __attribute__((aligned(16)));
	unsigned int peek_target = GET_JUMP_TARGET(_lw((unsigned int) sceCtrlPeekLatch));
	unsigned int read_target = GET_JUMP_TARGET(_lw((unsigned int) sceCtrlReadLatch));

	rjmLogHex("ctrl peek latch target ", peek_target);
	rjmLogHex("ctrl read latch target ", read_target);
	patch_function_jump(peek_target, ctrl_peek_latch_hook,
		(void **) &g_ctrl_peek_latch_common, peek_trampoline);
	patch_function_jump(read_target, ctrl_read_latch_hook,
		(void **) &g_ctrl_read_latch_common, read_trampoline);
}

static void hook_ctrl_buffer_entry_functions(void)
{
	static unsigned int peek_positive_trampoline[5] __attribute__((aligned(16)));
	static unsigned int peek_negative_trampoline[5] __attribute__((aligned(16)));
	static unsigned int read_positive_trampoline[5] __attribute__((aligned(16)));
	static unsigned int read_negative_trampoline[5] __attribute__((aligned(16)));
	unsigned int peek_positive_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlPeekBufferPositive));
	unsigned int peek_negative_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlPeekBufferNegative));
	unsigned int read_positive_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlReadBufferPositive));
	unsigned int read_negative_target =
		GET_JUMP_TARGET(_lw((unsigned int) sceCtrlReadBufferNegative));

	rjmLogHex("ctrl peek pos target ", peek_positive_target);
	rjmLogHex("ctrl peek neg target ", peek_negative_target);
	rjmLogHex("ctrl read pos target ", read_positive_target);
	rjmLogHex("ctrl read neg target ", read_negative_target);
	patch_function_jump(peek_positive_target, ctrl_peek_positive_hook,
		(void **) &g_ctrl_peek_positive_common, peek_positive_trampoline);
	patch_function_jump(peek_negative_target, ctrl_peek_negative_hook,
		(void **) &g_ctrl_peek_negative_common, peek_negative_trampoline);
	patch_function_jump(read_positive_target, ctrl_read_positive_hook,
		(void **) &g_ctrl_read_positive_common, read_positive_trampoline);
	patch_function_jump(read_negative_target, ctrl_read_negative_hook,
		(void **) &g_ctrl_read_negative_common, read_negative_trampoline);
}

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

static void handle_joyevent(const struct JoyEvent *joyevent)
{
	int intc = pspSdkDisableInterrupts();
	unsigned int old_buttons = g_currjoy.Buttons;
	unsigned int new_buttons = old_buttons;
	unsigned int old_inject;
	unsigned int new_inject;

	switch(joyevent->type)
	{
		case TYPE_BUTTON_UP:
			new_buttons &= ~joyevent->value;
			break;
		case TYPE_BUTTON_DOWN:
			new_buttons |= joyevent->value;
			break;
		case TYPE_ANALOG_Y:
			g_currjoy.Ly = joyevent->value;
			break;
		case TYPE_ANALOG_X:
			g_currjoy.Lx = joyevent->value;
			break;
		default:
			break;
	}
	g_currjoy.Buttons = new_buttons;
	old_inject = sanitize_remote_buttons(old_buttons);
	new_inject = sanitize_remote_buttons(new_buttons);
	g_remote_latch_make |= (~old_inject & new_inject);
	g_remote_latch_break |= (old_inject & ~new_inject);
	if(joyevent->type == TYPE_BUTTON_UP || joyevent->type == TYPE_BUTTON_DOWN)
	{
		rjmLogHex("joy buttons ", new_buttons);
	}
	else if(joyevent->type == TYPE_ANALOG_X || joyevent->type == TYPE_ANALOG_Y)
	{
		rjmLogHex("joy analog type ", joyevent->type);
		rjmLogHex("joy analog value ", joyevent->value);
	}

	pspSdkEnableInterrupts(intc);
	update_system_button_intercepts(g_currjoy.Buttons);
}

static int main_thread(SceSize args, void *argp)
{
	rjmLogText("main_thread begin\n");
	hook_ctrl_buffer_entry_functions();
	rjmLogText("ctrl hook ok\n");
	hook_ctrl_latch_functions();

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
		sceKernelExitDeleteThread(0);
	}
	rjmLogText("start usb ok\n");

	rjmLogText("usb suspend/resume\n");
	rjmUsbSuspend();
	rjmUsbResume();
	rjmUsbAsyncFlush();

	while(1)
	{
		struct JoyEvent joyevent;
		int len;

		reset_remote_input_state();
		rjmLogText("waiting connect\n");
		rjmUsbWaitForConnect();
		rjmLogText("connected\n");
		rjmUsbAsyncFlush();

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
	reserve_pops_reboot_load();
	memset(&g_currjoy, 0, sizeof(g_currjoy));
	g_currjoy.Lx = ANALOG_CENTER;
	g_currjoy.Ly = ANALOG_CENTER;
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
	if(g_cleanup_started)
	{
		rjmLogText("cleanup already started\n");
		return;
	}
	g_cleanup_started = 1;

	rjmLogText(reason);
	rjmLogHex("cleanup apitype ", sceKernelInitApitype());
	rjmLogHex("cleanup keyconfig ", sceKernelApplicationType());
	rjmLogHex("cleanup scePops_Manager ", (int) sceKernelFindModuleByName("scePops_Manager"));
	g_special_thread_run = 0;
	rjmLogText("cleanup reset input\n");
	reset_remote_input_state();

	if(g_intercept_buttons)
	{
		unsigned int k1 = psplinkSetK1(0);
		sceCtrlSetButtonIntercept(g_intercept_buttons, SCE_CTRL_MASK_NO_MASK);
		psplinkSetK1(k1);
		g_intercept_buttons = 0;
	}
	rjmLogText("cleanup intercept cleared\n");

	if(g_main_thid >= 0)
	{
		rjmLogHex("cleanup delete main ", g_main_thid);
		sceKernelTerminateDeleteThread(g_main_thid);
		g_main_thid = -1;
	}
	if(g_special_thid >= 0)
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
	if(g_power_thid >= 0)
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

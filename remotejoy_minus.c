/*
 * remotejoy_minus.c - input-only RemoteJoyMinus USB device layer
 *
 * This keeps the RemoteJoy/usbhostfs wire layout used by the RP2040-zero
 * host firmware:
 * EP1 bulk IN for HostFS hello, EP2 bulk OUT for commands, EP3 bulk OUT for
 * async JoyEvent packets.
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <string.h>
#include <systemctrl.h>
#include "remotejoy_minus.h"
#include "rjm_log.h"

#define HOSTFSDRIVER_NAME "RemoteJoyMinusUSB"
#define HOSTFSDRIVER_PID  0x1C9
#define HOSTFS_MAGIC      0x782F0812
#define ASYNC_MAGIC       0x782F0813
#define HOSTFS_CMD_HELLO  0x8FFC0000
#define MAX_ASYNC_BUFFER  4096
#define PSP_USB_ERROR_ALREADY_STARTED 0x80243001

enum UsbEvents
{
	USB_EVENT_ATTACH  = 1,
	USB_EVENT_DETACH  = 2,
	USB_EVENT_ASYNC   = 4,
	USB_EVENT_CONNECT = 8
};

enum UsbTransEvents
{
	USB_TRANSEVENT_BULKOUT_DONE = 1,
	USB_TRANSEVENT_BULKIN_DONE  = 2
};

struct HostFsCmd
{
	unsigned int magic;
	unsigned int command;
	unsigned int extralen;
} __attribute__((packed));

struct AsyncCommand
{
	unsigned int magic;
	unsigned int channel;
} __attribute__((packed));

struct AsyncEndpoint
{
	unsigned char buffer[MAX_ASYNC_BUFFER];
	int read_pos;
	int write_pos;
	int size;
};

static struct DeviceDescriptor g_devdesc_hi =
{
	18, 1, 0x200, 0, 0, 0, 64, 0, 0, 0x100, 0, 0, 0, 1
};

static struct ConfigDescriptor g_confdesc_hi =
{
	9, 2, (9 + 9 + (3 * 7)), 1, 1, 0, 0xC0, 0
};

static struct InterfaceDescriptor g_interdesc_hi =
{
	9, 4, 0, 0, 3, 0xFF, 0x01, 0xFF, 1
};

static struct EndpointDescriptor g_endpdesc_hi[3] =
{
	{ 7, 5, 0x81, 2, 512, 0 },
	{ 7, 5, 0x02, 2, 512, 0 },
	{ 7, 5, 0x03, 2, 512, 0 }
};

static struct DeviceDescriptor g_devdesc_full =
{
	18, 1, 0x200, 0, 0, 0, 64, 0, 0, 0x100, 0, 0, 0, 1
};

static struct ConfigDescriptor g_confdesc_full =
{
	9, 2, (9 + 9 + (3 * 7)), 1, 1, 0, 0xC0, 0
};

static struct InterfaceDescriptor g_interdesc_full =
{
	9, 4, 0, 0, 3, 0xFF, 0x01, 0xFF, 1
};

static struct EndpointDescriptor g_endpdesc_full[3] =
{
	{ 7, 5, 0x81, 2, 64, 0 },
	{ 7, 5, 0x02, 2, 64, 0 },
	{ 7, 5, 0x03, 2, 64, 0 }
};

static struct StringDescriptor g_string_desc = { 0x8, 0x3, { '<', '>', 0 } };
static struct UsbEndpoint g_endp[4] = { {0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0} };
static struct UsbInterface g_intp = { 0xFFFFFFFF, 0, 1 };
static struct UsbData g_usbdata[2];

static SceUID g_usb_thread = -1;
static SceUID g_main_event = -1;
static SceUID g_trans_event = -1;
static SceUID g_async_event = -1;
static SceUID g_bus_sema = -1;
static struct UsbdDeviceReq g_bulkin_req;
static struct UsbdDeviceReq g_bulkout_req;
static struct UsbdDeviceReq g_async_req;
static struct AsyncEndpoint g_async_endp;
static int g_usb_started;
static int g_usb_ready;
static unsigned char g_transfer_buf[64 * 1024] __attribute__((aligned(64)));
static unsigned char g_async_buf[512] __attribute__((aligned(64)));
static int (*g_sceUsbStart)(const char *, int, void *);
static int (*g_sceUsbStop)(const char *, int, void *);
static int (*g_sceUsbActivate)(u32);
static int (*g_sceUsbDeactivate)(u32);
static int (*g_sceUsbbdRegister)(struct UsbDriver *);
static int (*g_sceUsbbdUnregister)(struct UsbDriver *);
static int (*g_sceUsbbdReqSend)(struct UsbdDeviceReq *);
static int (*g_sceUsbbdReqRecv)(struct UsbdDeviceReq *);
static int (*g_sceUsbbdReqCancelAll)(struct UsbEndpoint *);
static int (*g_sceUsbbdClearFIFO)(struct UsbEndpoint *);

static int usb_thread(SceSize size, void *argp);

static u32 find_usb_function(const char *library, u32 nid)
{
	u32 addr;

	addr = sctrlHENFindFunction("sceUSB_Driver", library, nid);
	if(addr != 0)
	{
		return addr;
	}

	addr = sctrlHENFindFunction("sceUsb", library, nid);
	if(addr != 0)
	{
		return addr;
	}

	return sctrlHENFindFunction("sceUsbBus", library, nid);
}

static int resolve_usb_functions(void)
{
	if(g_sceUsbStart != NULL)
	{
		return 0;
	}

	g_sceUsbStart = (void *) find_usb_function("sceUsb", 0xAE5DE6AF);
	g_sceUsbStop = (void *) find_usb_function("sceUsb", 0xC2464FA0);
	g_sceUsbActivate = (void *) find_usb_function("sceUsb", 0x586DB82C);
	g_sceUsbDeactivate = (void *) find_usb_function("sceUsb", 0xC572A9C8);
	g_sceUsbbdRegister = (void *) find_usb_function("sceUsbbd", 0xB1644BE7);
	g_sceUsbbdUnregister = (void *) find_usb_function("sceUsbbd", 0xC1E2A540);
	g_sceUsbbdReqSend = (void *) find_usb_function("sceUsbbd", 0x23E51D8F);
	g_sceUsbbdReqRecv = (void *) find_usb_function("sceUsbbd", 0x913EC15D);
	g_sceUsbbdReqCancelAll = (void *) find_usb_function("sceUsbbd", 0xC5E53685);
	g_sceUsbbdClearFIFO = (void *) find_usb_function("sceUsbbd", 0x951A24CC);

	rjmLogHex("resolve sceUsbStart ", (int) g_sceUsbStart);
	rjmLogHex("resolve sceUsbStop ", (int) g_sceUsbStop);
	rjmLogHex("resolve sceUsbActivate ", (int) g_sceUsbActivate);
	rjmLogHex("resolve sceUsbDeactivate ", (int) g_sceUsbDeactivate);
	rjmLogHex("resolve sceUsbbdRegister ", (int) g_sceUsbbdRegister);
	rjmLogHex("resolve sceUsbbdUnregister ", (int) g_sceUsbbdUnregister);
	rjmLogHex("resolve sceUsbbdReqSend ", (int) g_sceUsbbdReqSend);
	rjmLogHex("resolve sceUsbbdReqRecv ", (int) g_sceUsbbdReqRecv);

	if(g_sceUsbbdRegister == NULL)
	{
		g_sceUsbbdRegister = (void *) find_usb_function("sceUsbBus_driver", 0xB1644BE7);
		g_sceUsbbdUnregister = (void *) find_usb_function("sceUsbBus_driver", 0xC1E2A540);
		g_sceUsbbdReqSend = (void *) find_usb_function("sceUsbBus_driver", 0x23E51D8F);
		g_sceUsbbdReqRecv = (void *) find_usb_function("sceUsbBus_driver", 0x913EC15D);
		g_sceUsbbdReqCancelAll = (void *) find_usb_function("sceUsbBus_driver", 0xC5E53685);
		g_sceUsbbdClearFIFO = (void *) find_usb_function("sceUsbBus_driver", 0x951A24CC);
	}
	rjmLogHex("resolve fallback sceUsbbdRegister ", (int) g_sceUsbbdRegister);
	rjmLogHex("resolve fallback sceUsbbdUnregister ", (int) g_sceUsbbdUnregister);
	rjmLogHex("resolve fallback sceUsbbdReqSend ", (int) g_sceUsbbdReqSend);
	rjmLogHex("resolve fallback sceUsbbdReqRecv ", (int) g_sceUsbbdReqRecv);
	rjmLogHex("resolve fallback sceUsbbdReqCancelAll ", (int) g_sceUsbbdReqCancelAll);
	rjmLogHex("resolve fallback sceUsbbdClearFIFO ", (int) g_sceUsbbdClearFIFO);

	if(g_sceUsbStart == NULL || g_sceUsbStop == NULL ||
		g_sceUsbActivate == NULL || g_sceUsbDeactivate == NULL ||
		g_sceUsbbdRegister == NULL || g_sceUsbbdUnregister == NULL ||
		g_sceUsbbdReqSend == NULL || g_sceUsbbdReqRecv == NULL)
	{
		return -1;
	}

	return 0;
}

static int usb_request(int arg1, int arg2, struct DeviceRequest *req)
{
	return 0;
}

static int usb_unknown(int arg1, int arg2, int arg3)
{
	return 0;
}

static int usb_attach(int speed, void *arg2, void *arg3)
{
	sceKernelSetEventFlag(g_main_event, USB_EVENT_ATTACH);
	return 0;
}

static int usb_detach(int arg1, int arg2, int arg3)
{
	g_usb_ready = 0;
	sceKernelClearEventFlag(g_main_event, ~USB_EVENT_CONNECT);
	sceKernelSetEventFlag(g_async_event, 1 << RJM_ASYNC_JOY);
	sceKernelSetEventFlag(g_main_event, USB_EVENT_DETACH);
	return 0;
}

static int bulkin_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
	sceKernelSetEventFlag(g_trans_event, USB_TRANSEVENT_BULKIN_DONE);
	return 0;
}

static int bulkout_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
	sceKernelSetEventFlag(g_trans_event, USB_TRANSEVENT_BULKOUT_DONE);
	return 0;
}

static int async_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
	sceKernelSetEventFlag(g_main_event, USB_EVENT_ASYNC);
	return 0;
}

static int set_bulkin_req(void *data, int size)
{
	sceKernelDcacheWritebackRange(data, size);
	memset(&g_bulkin_req, 0, sizeof(g_bulkin_req));
	g_bulkin_req.endp = &g_endp[1];
	g_bulkin_req.data = data;
	g_bulkin_req.size = size;
	g_bulkin_req.func = bulkin_done;
	sceKernelClearEventFlag(g_trans_event, ~USB_TRANSEVENT_BULKIN_DONE);
	if(g_sceUsbbdReqSend == NULL)
	{
		return -1;
	}
	return g_sceUsbbdReqSend(&g_bulkin_req);
}

static int set_bulkout_req(void *data, int size)
{
	unsigned int addr = (unsigned int) data;
	unsigned int blockaddr = addr & ~63;
	unsigned int topaddr = (addr + size + 63) & ~63;

	if(blockaddr != addr)
	{
		return -1;
	}

	sceKernelDcacheInvalidateRange((void *) blockaddr, topaddr - blockaddr);
	memset(&g_bulkout_req, 0, sizeof(g_bulkout_req));
	g_bulkout_req.endp = &g_endp[2];
	g_bulkout_req.data = data;
	g_bulkout_req.size = size;
	g_bulkout_req.func = bulkout_done;
	sceKernelClearEventFlag(g_trans_event, ~USB_TRANSEVENT_BULKOUT_DONE);
	if(g_sceUsbbdReqRecv == NULL)
	{
		return -1;
	}
	return g_sceUsbbdReqRecv(&g_bulkout_req);
}

static int set_async_req(void *data, int size)
{
	unsigned int addr = (unsigned int) data;
	unsigned int blockaddr = addr & ~63;
	unsigned int topaddr = (addr + size + 63) & ~63;

	if(blockaddr != addr)
	{
		return -1;
	}

	sceKernelDcacheInvalidateRange((void *) blockaddr, topaddr - blockaddr);
	memset(&g_async_req, 0, sizeof(g_async_req));
	g_async_req.endp = &g_endp[3];
	g_async_req.data = data;
	g_async_req.size = size;
	g_async_req.func = async_done;
	sceKernelClearEventFlag(g_main_event, ~USB_EVENT_ASYNC);
	if(g_sceUsbbdReqRecv == NULL)
	{
		return -1;
	}
	return g_sceUsbbdReqRecv(&g_async_req);
}

static int read_data(void *data, int size)
{
	int readlen = 0;

	while(readlen < size)
	{
		int nextsize = (size - readlen) > sizeof(g_transfer_buf) ?
			sizeof(g_transfer_buf) : (size - readlen);
		u32 result;
		int ret;

		if(set_bulkout_req(g_transfer_buf, nextsize) < 0)
		{
			return -1;
		}

		ret = sceKernelWaitEventFlag(g_trans_event,
			USB_TRANSEVENT_BULKOUT_DONE,
			PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result, NULL);
		if(ret < 0 || g_bulkout_req.retcode != 0 || g_bulkout_req.recvsize <= 0)
		{
			return -1;
		}

		memcpy(data, g_transfer_buf, g_bulkout_req.recvsize);
		data += g_bulkout_req.recvsize;
		readlen += g_bulkout_req.recvsize;
	}

	return readlen;
}

static int write_data(const void *data, int size)
{
	int writelen = 0;

	while(writelen < size)
	{
		int nextsize = (size - writelen) > sizeof(g_transfer_buf) ?
			sizeof(g_transfer_buf) : (size - writelen);
		const void *send_data = data;
		u32 result;
		int ret;

		if((unsigned int) data & 63)
		{
			memcpy(g_transfer_buf, data, nextsize);
			send_data = g_transfer_buf;
		}

		if(set_bulkin_req((void *) send_data, nextsize) < 0)
		{
			return -1;
		}

		ret = sceKernelWaitEventFlag(g_trans_event,
			USB_TRANSEVENT_BULKIN_DONE,
			PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result, NULL);
		if(ret < 0 || g_bulkin_req.retcode != 0 || g_bulkin_req.recvsize <= 0)
		{
			return -1;
		}

		writelen += g_bulkin_req.recvsize;
		data += g_bulkin_req.recvsize;
	}

	return writelen;
}

static int command_xchg(void *outcmd, int outcmdlen, void *incmd, int incmdlen)
{
	int ret = 0;

	if(sceKernelWaitSema(g_bus_sema, 1, NULL) < 0)
	{
		return 0;
	}

	do
	{
		struct HostFsCmd *cmd = (struct HostFsCmd *) outcmd;
		struct HostFsCmd *resp = (struct HostFsCmd *) incmd;

		if(outcmdlen > 0 && write_data(outcmd, outcmdlen) != outcmdlen)
		{
			break;
		}
		if(incmdlen > 0)
		{
			if(read_data(incmd, incmdlen) != incmdlen)
			{
				break;
			}
			if(resp->magic != HOSTFS_MAGIC || resp->command != cmd->command)
			{
				break;
			}
		}

		ret = 1;
	}
	while(0);

	sceKernelSignalSema(g_bus_sema, 1);
	return ret;
}

static int send_hello_cmd(void)
{
	struct HostFsCmd cmd;
	struct HostFsCmd resp;

	memset(&cmd, 0, sizeof(cmd));
	memset(&resp, 0, sizeof(resp));
	cmd.magic = HOSTFS_MAGIC;
	cmd.command = HOSTFS_CMD_HELLO;

	return command_xchg(&cmd, sizeof(cmd), &resp, sizeof(resp));
}

static void fill_async(void *async_data, int len)
{
	struct AsyncCommand *cmd;
	unsigned char *data;
	int sizeleft;
	int intc;

	if(len <= sizeof(struct AsyncCommand))
	{
		return;
	}

	cmd = (struct AsyncCommand *) async_data;
	data = async_data + sizeof(struct AsyncCommand);
	len -= sizeof(struct AsyncCommand);

	if(cmd->magic != ASYNC_MAGIC || cmd->channel != RJM_ASYNC_JOY)
	{
		return;
	}

	intc = pspSdkDisableInterrupts();
	sizeleft = len < (MAX_ASYNC_BUFFER - g_async_endp.size) ?
		len : (MAX_ASYNC_BUFFER - g_async_endp.size);
	while(sizeleft > 0)
	{
		g_async_endp.buffer[g_async_endp.write_pos++] = *data++;
		g_async_endp.write_pos %= MAX_ASYNC_BUFFER;
		g_async_endp.size++;
		sizeleft--;
	}
	sceKernelSetEventFlag(g_async_event, 1 << RJM_ASYNC_JOY);
	pspSdkEnableInterrupts(intc);
}

void rjmUsbAsyncFlush(void)
{
	int intc;

	if(g_async_event < 0)
	{
		return;
	}

	intc = pspSdkDisableInterrupts();
	g_async_endp.size = 0;
	g_async_endp.read_pos = 0;
	g_async_endp.write_pos = 0;
	sceKernelClearEventFlag(g_async_event, ~(1 << RJM_ASYNC_JOY));
	pspSdkEnableInterrupts(intc);
}

int rjmUsbAsyncRead(unsigned char *data, int len)
{
	int ret;
	int intc;
	int i;

	if(!g_usb_ready)
	{
		return -1;
	}

	ret = sceKernelWaitEventFlag(g_async_event, 1 << RJM_ASYNC_JOY,
		PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, NULL, NULL);
	if(ret < 0 || !g_usb_ready)
	{
		return -1;
	}

	intc = pspSdkDisableInterrupts();
	len = len < g_async_endp.size ? len : g_async_endp.size;
	for(i = 0; i < len; i++)
	{
		data[i] = g_async_endp.buffer[g_async_endp.read_pos++];
		g_async_endp.read_pos %= MAX_ASYNC_BUFFER;
		g_async_endp.size--;
	}
	if(g_async_endp.size != 0)
	{
		sceKernelSetEventFlag(g_async_event, 1 << RJM_ASYNC_JOY);
	}
	pspSdkEnableInterrupts(intc);

	return len;
}

static int usb_thread(SceSize size, void *argp)
{
	while(1)
	{
		u32 result;
		int ret;

		ret = sceKernelWaitEventFlag(g_main_event,
			USB_EVENT_ATTACH | USB_EVENT_DETACH | USB_EVENT_ASYNC,
			PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result, NULL);
		if(ret < 0)
		{
			sceKernelExitDeleteThread(0);
		}

		if(result & USB_EVENT_ASYNC)
		{
			if(g_async_req.retcode == 0 && g_async_req.recvsize > 0)
			{
				fill_async(g_async_buf, g_async_req.recvsize);
				if(g_usb_ready)
				{
					set_async_req(g_async_buf, sizeof(g_async_buf));
				}
			}
		}

		if(result & USB_EVENT_DETACH)
		{
			g_usb_ready = 0;
			rjmUsbAsyncFlush();
			sceKernelSetEventFlag(g_async_event, 1 << RJM_ASYNC_JOY);
		}

		if(result & USB_EVENT_ATTACH)
		{
			unsigned int magic;

			g_usb_ready = 0;
			rjmUsbAsyncFlush();
			sceKernelClearEventFlag(g_main_event, ~USB_EVENT_CONNECT);

			if(read_data(&magic, sizeof(magic)) == sizeof(magic) &&
				magic == HOSTFS_MAGIC && send_hello_cmd())
			{
				set_async_req(g_async_buf, sizeof(g_async_buf));
				g_usb_ready = 1;
				sceKernelSetEventFlag(g_main_event, USB_EVENT_CONNECT);
			}
		}
	}

	return 0;
}

static int start_func(int size, void *p)
{
	memset(g_usbdata, 0, sizeof(g_usbdata));
	memcpy(g_usbdata[0].devdesc, &g_devdesc_hi, sizeof(g_devdesc_hi));
	g_usbdata[0].config.pconfdesc = &g_usbdata[0].confdesc;
	g_usbdata[0].config.pinterfaces = &g_usbdata[0].interfaces;
	g_usbdata[0].config.pinterdesc = &g_usbdata[0].interdesc;
	g_usbdata[0].config.pendp = g_usbdata[0].endp;
	memcpy(g_usbdata[0].confdesc.desc, &g_confdesc_hi, sizeof(g_confdesc_hi));
	g_usbdata[0].confdesc.pinterfaces = &g_usbdata[0].interfaces;
	g_usbdata[0].interfaces.pinterdesc[0] = &g_usbdata[0].interdesc;
	g_usbdata[0].interfaces.intcount = 1;
	memcpy(g_usbdata[0].interdesc.desc, &g_interdesc_hi, sizeof(g_interdesc_hi));
	g_usbdata[0].interdesc.pendp = g_usbdata[0].endp;
	memcpy(g_usbdata[0].endp[0].desc, &g_endpdesc_hi[0], sizeof(g_endpdesc_hi[0]));
	memcpy(g_usbdata[0].endp[1].desc, &g_endpdesc_hi[1], sizeof(g_endpdesc_hi[1]));
	memcpy(g_usbdata[0].endp[2].desc, &g_endpdesc_hi[2], sizeof(g_endpdesc_hi[2]));

	memcpy(g_usbdata[1].devdesc, &g_devdesc_full, sizeof(g_devdesc_full));
	g_usbdata[1].config.pconfdesc = &g_usbdata[1].confdesc;
	g_usbdata[1].config.pinterfaces = &g_usbdata[1].interfaces;
	g_usbdata[1].config.pinterdesc = &g_usbdata[1].interdesc;
	g_usbdata[1].config.pendp = g_usbdata[1].endp;
	memcpy(g_usbdata[1].confdesc.desc, &g_confdesc_full, sizeof(g_confdesc_full));
	g_usbdata[1].confdesc.pinterfaces = &g_usbdata[1].interfaces;
	g_usbdata[1].interfaces.pinterdesc[0] = &g_usbdata[1].interdesc;
	g_usbdata[1].interfaces.intcount = 1;
	memcpy(g_usbdata[1].interdesc.desc, &g_interdesc_full, sizeof(g_interdesc_full));
	g_usbdata[1].interdesc.pendp = g_usbdata[1].endp;
	memcpy(g_usbdata[1].endp[0].desc, &g_endpdesc_full[0], sizeof(g_endpdesc_full[0]));
	memcpy(g_usbdata[1].endp[1].desc, &g_endpdesc_full[1], sizeof(g_endpdesc_full[1]));
	memcpy(g_usbdata[1].endp[2].desc, &g_endpdesc_full[2], sizeof(g_endpdesc_full[2]));

	g_main_event = sceKernelCreateEventFlag("RJMUsbMain", 0x200, 0, NULL);
	g_trans_event = sceKernelCreateEventFlag("RJMUsbTrans", 0x200, 0, NULL);
	g_async_event = sceKernelCreateEventFlag("RJMUsbAsync", 0x200, 0, NULL);
	g_bus_sema = sceKernelCreateSema("RJMUsbBus", 0, 1, 1, NULL);
	if(g_main_event < 0 || g_trans_event < 0 || g_async_event < 0 || g_bus_sema < 0)
	{
		return -1;
	}

	rjmUsbAsyncFlush();
	g_usb_ready = 0;
	g_usb_thread = sceKernelCreateThread("RJMUsbThread", usb_thread, 10, 0x10000, 0, NULL);
	if(g_usb_thread < 0)
	{
		return -1;
	}
	if(sceKernelStartThread(g_usb_thread, 0, NULL) < 0)
	{
		return -1;
	}

	g_usb_started = 1;
	return 0;
}

static int stop_func(int size, void *p)
{
	g_usb_ready = 0;
	g_usb_started = 0;
	if(g_usb_thread >= 0)
	{
		sceKernelTerminateDeleteThread(g_usb_thread);
		g_usb_thread = -1;
	}
	if(g_main_event >= 0)
	{
		sceKernelDeleteEventFlag(g_main_event);
		g_main_event = -1;
	}
	if(g_trans_event >= 0)
	{
		sceKernelDeleteEventFlag(g_trans_event);
		g_trans_event = -1;
	}
	if(g_async_event >= 0)
	{
		sceKernelDeleteEventFlag(g_async_event);
		g_async_event = -1;
	}
	if(g_bus_sema >= 0)
	{
		sceKernelDeleteSema(g_bus_sema);
		g_bus_sema = -1;
	}

	return 0;
}

static struct UsbDriver g_driver =
{
	HOSTFSDRIVER_NAME,
	4,
	g_endp,
	&g_intp,
	g_usbdata[0].devdesc,
	&g_usbdata[0].config,
	g_usbdata[1].devdesc,
	&g_usbdata[1].config,
	&g_string_desc,
	usb_request,
	usb_unknown,
	usb_attach,
	usb_detach,
	0,
	start_func,
	stop_func,
	NULL
};

void rjmUsbRegister(void)
{
	if(resolve_usb_functions() == 0)
	{
		rjmLogHex("sceUsbbdRegister ", g_sceUsbbdRegister(&g_driver));
	}
	else
	{
		rjmLogText("resolve_usb_functions failed in register\n");
	}
}

void rjmUsbUnregister(void)
{
	if(g_sceUsbbdUnregister != NULL)
	{
		rjmLogHex("sceUsbbdUnregister ", g_sceUsbbdUnregister(&g_driver));
	}
}

int rjmUsbStart(void)
{
	int ret;

	if(resolve_usb_functions() < 0)
	{
		rjmLogText("resolve_usb_functions failed in start\n");
		return -1;
	}
	ret = g_sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
	rjmLogHex("sceUsbStart bus ", ret);
	if(ret != 0 && ret != PSP_USB_ERROR_ALREADY_STARTED)
	{
		return -1;
	}
	ret = g_sceUsbStart(HOSTFSDRIVER_NAME, 0, 0);
	rjmLogHex("sceUsbStart rjm ", ret);
	if(ret != 0 && ret != PSP_USB_ERROR_ALREADY_STARTED)
	{
		return -1;
	}
	ret = g_sceUsbActivate(HOSTFSDRIVER_PID);
	rjmLogHex("sceUsbActivate ", ret);
	if(ret != 0)
	{
		return -1;
	}

	return 0;
}

int rjmUsbStop(void)
{
	if(g_sceUsbDeactivate != NULL)
	{
		g_sceUsbDeactivate(HOSTFSDRIVER_PID);
	}
	if(g_sceUsbStop != NULL)
	{
		g_sceUsbStop(HOSTFSDRIVER_NAME, 0, 0);
	}
	return 0;
}

void rjmUsbShutdown(void)
{
	int i;

	rjmLogText("usb_shutdown begin\n");
	g_usb_ready = 0;
	if(g_async_event >= 0)
	{
		rjmLogText("usb_shutdown wake async\n");
		sceKernelSetEventFlag(g_async_event, 1 << RJM_ASYNC_JOY);
	}
	if(g_main_event >= 0)
	{
		rjmLogText("usb_shutdown wake main\n");
		sceKernelSetEventFlag(g_main_event,
			USB_EVENT_DETACH | USB_EVENT_ASYNC | USB_EVENT_CONNECT);
	}
	sceKernelDelayThread(50000);

	if(g_sceUsbbdReqCancelAll != NULL)
	{
		for(i = 0; i < 4; i++)
		{
			rjmLogHex("usb_shutdown cancel ep ", i);
			rjmLogHex("usb_shutdown cancel ret ", g_sceUsbbdReqCancelAll(&g_endp[i]));
		}
	}
	else
	{
		rjmLogText("usb_shutdown cancel missing\n");
	}
	sceKernelDelayThread(50000);

	if(g_sceUsbbdClearFIFO != NULL)
	{
		for(i = 0; i < 4; i++)
		{
			rjmLogHex("usb_shutdown clear ep ", i);
			rjmLogHex("usb_shutdown clear ret ", g_sceUsbbdClearFIFO(&g_endp[i]));
		}
	}
	else
	{
		rjmLogText("usb_shutdown clear missing\n");
	}
	sceKernelDelayThread(50000);

	rjmLogText("usb_shutdown stop\n");
	rjmUsbStop();
	rjmLogText("usb_shutdown stop done\n");
	sceKernelDelayThread(50000);

	if(g_usb_thread >= 0)
	{
		rjmLogHex("usb_shutdown delete usb_thread ", g_usb_thread);
		sceKernelTerminateDeleteThread(g_usb_thread);
		g_usb_thread = -1;
	}
	if(g_main_event >= 0)
	{
		rjmLogText("usb_shutdown delete main_event\n");
		sceKernelDeleteEventFlag(g_main_event);
		g_main_event = -1;
	}
	if(g_trans_event >= 0)
	{
		rjmLogText("usb_shutdown delete trans_event\n");
		sceKernelDeleteEventFlag(g_trans_event);
		g_trans_event = -1;
	}
	if(g_async_event >= 0)
	{
		rjmLogText("usb_shutdown delete async_event\n");
		sceKernelDeleteEventFlag(g_async_event);
		g_async_event = -1;
	}
	if(g_bus_sema >= 0)
	{
		rjmLogText("usb_shutdown delete sema\n");
		sceKernelDeleteSema(g_bus_sema);
		g_bus_sema = -1;
	}

	rjmLogText("usb_shutdown unregister\n");
	rjmUsbUnregister();
	rjmLogText("usb_shutdown done\n");
}

int rjmUsbSuspend(void)
{
	if(!g_usb_started)
	{
		return -1;
	}
	if(g_sceUsbDeactivate == NULL)
	{
		return -1;
	}
	g_usb_ready = 0;
	if(g_async_event >= 0)
	{
		sceKernelSetEventFlag(g_async_event, 1 << RJM_ASYNC_JOY);
	}
	if(g_main_event >= 0)
	{
		sceKernelClearEventFlag(g_main_event, ~USB_EVENT_CONNECT);
		sceKernelSetEventFlag(g_main_event, USB_EVENT_DETACH);
	}
	return g_sceUsbDeactivate(HOSTFSDRIVER_PID);
}

int rjmUsbResume(void)
{
	if(g_sceUsbActivate == NULL)
	{
		return -1;
	}
	return g_sceUsbActivate(HOSTFSDRIVER_PID);
}

int rjmUsbWaitForConnect(void)
{
	while(g_main_event < 0)
	{
		sceKernelDelayThread(100000);
	}

	return sceKernelWaitEventFlag(g_main_event, USB_EVENT_CONNECT,
		PSP_EVENT_WAITOR, NULL, NULL) == 0;
}

int rjmUsbIsConnected(void)
{
	return g_usb_ready;
}

#include <pspkernel.h>
#include <pspiofilemgr_kernel.h>
#include <string.h>
#include "rjm_log.h"

#ifndef RJM_ENABLE_LOG
#define RJM_ENABLE_LOG 0
#endif

#ifndef RJM_TRACE_PACE_US
#define RJM_TRACE_PACE_US 10000
#endif

#if RJM_ENABLE_LOG
static int g_log_initialized;

static int str_len(const char *s)
{
	int len = 0;

	while(s && s[len])
	{
		len++;
	}

	return len;
}

void rjmLogInit(void)
{
	SceUID fd;

	if(g_log_initialized)
	{
		return;
	}

	fd = sceIoOpen("ms0:/rjm_standalone.log",
		PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0)
	{
		sceIoWrite(fd, "=== rjm standalone session ===\n", 31);
		sceIoClose(fd);
	}
	g_log_initialized = 1;
}

void rjmLogText(const char *text)
{
	SceUID fd;

	rjmLogInit();

	fd = sceIoOpen("ms0:/rjm_standalone.log",
		PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0)
	{
		sceIoWrite(fd, text, str_len(text));
		sceIoClose(fd);
	}
}

void rjmLogHex(const char *label, int value)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[96];
	int pos = 0;
	int i;
	SceUID fd;

	rjmLogInit();

	while(label && *label && pos < (int) sizeof(buf) - 12)
	{
		buf[pos++] = *label++;
	}
	buf[pos++] = '0';
	buf[pos++] = 'x';
	for(i = 0; i < 8; i++)
	{
		buf[pos++] = hex[((unsigned int) value >> ((7 - i) * 4)) & 0xF];
	}
	buf[pos++] = '\n';

	fd = sceIoOpen("ms0:/rjm_standalone.log",
		PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0)
	{
		sceIoWrite(fd, buf, pos);
		sceIoClose(fd);
	}
}
#else
static void trace_pace(void)
{
#if RJM_TRACE_PACE_US > 0
	sceKernelDelayThread(RJM_TRACE_PACE_US);
#endif
}

void rjmLogInit(void)
{
	trace_pace();
}

void rjmLogText(const char *text)
{
	(void) text;
	trace_pace();
}

void rjmLogHex(const char *label, int value)
{
	(void) label;
	(void) value;
	trace_pace();
}
#endif

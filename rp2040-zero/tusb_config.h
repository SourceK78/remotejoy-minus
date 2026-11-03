#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU OPT_MCU_RP2040
#define CFG_TUSB_OS OPT_OS_PICO

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

/* The native USB port is a USB host to the PSP in normal mode, and a USB
 * network device to a PC in Web configuration mode. Both stacks are built;
 * main() initializes exactly one of them on rhport 0 at boot with
 * tusb_init(0, &init). No CFG_TUSB_RHPORT0_MODE, so tusb_init() without
 * arguments is unavailable on purpose. */
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

/* Host: PSP USBHostFS */
#define CFG_TUH_RPI_PIO_USB 0
#define CFG_TUSB_HOST_DEVICE_MAX 1

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 0
#define CFG_TUH_CDC 0
#define CFG_TUH_HID 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

#define CFG_TUH_ENDPOINT_MAX 8
#define CFG_TUH_API_EDPT_XFER 1

/* Device: CDC-NCM network interface for the Web configuration portal */
#include "lwipopts.h"

#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_NCM 1
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE (2 * TCP_MSS + 100)
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE (2 * TCP_MSS + 100)
#define CFG_TUD_NCM_IN_NTB_N 1
#define CFG_TUD_NCM_OUT_NTB_N 1

#endif

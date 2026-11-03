/*
 * usb_descriptors.c - USB CDC-NCM device descriptors for the Web
 * configuration mode. Based on the TinyUSB net_lwip_webserver example (MIT).
 */
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_MAC
};

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

#define EPNUM_NET_NOTIF 0x81
#define EPNUM_NET_OUT 0x02
#define EPNUM_NET_IN 0x82

static const tusb_desc_device_t k_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    /* 2.01 so the host asks for the BOS / MS OS 2.0 descriptors. */
    .bcdUSB = 0x0201,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,
    .idProduct = 0x4020,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&k_desc_device;
}

#define NCM_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)

static const uint8_t k_ncm_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NCM_CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64,
                           EPNUM_NET_OUT, EPNUM_NET_IN, CFG_TUD_NET_ENDPOINT_SIZE,
                           CFG_TUD_NET_MTU),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return index == 0 ? k_ncm_configuration : NULL;
}

/* Microsoft OS 2.0 descriptors bind the interface to the built-in Windows
 * NCM driver (WINNCM) without an INF. */
#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN 0xB2

static const uint8_t k_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)
};

const uint8_t *tud_descriptor_bos_cb(void)
{
    return k_desc_bos;
}

static const uint8_t k_desc_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_CDC, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'N', 'C', 'M', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, '1', 0x00, '2', 0x00, '3', 0x00, '4', 0x00, '5', 0x00, '6', 0x00, '7', 0x00,
    '8', 0x00, '-', 0x00, '0', 0x00, 'D', 0x00, '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00,
    '3', 0x00, 'F', 0x00, 'D', 0x00, '-', 0x00, '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00,
    '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00, '8', 0x00, 'A', 0x00,
    'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(k_desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == 1 && request->wIndex == 7) {
        uint16_t total_len;
        memcpy(&total_len, k_desc_ms_os_20 + 8, 2);
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)k_desc_ms_os_20, total_len);
    }
    return false;
}

extern uint8_t tud_network_mac_address[6];

static const char *const k_string_desc[] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "remotejoy-minus",
    [STRID_PRODUCT] = "RemoteJoy Config",
    [STRID_SERIAL] = NULL,
    [STRID_INTERFACE] = "RemoteJoy Config Network",
};

static uint16_t g_desc_str[32 + 1];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    unsigned chr_count = 0;
    (void)langid;

    switch (index) {
    case STRID_LANGID:
        memcpy(&g_desc_str[1], k_string_desc[0], 2);
        chr_count = 1;
        break;
    case STRID_SERIAL:
        chr_count = board_usb_get_serial(g_desc_str + 1, 32);
        break;
    case STRID_MAC:
        for (unsigned i = 0; i < 6; ++i) {
            g_desc_str[1 + chr_count++] = "0123456789ABCDEF"[(tud_network_mac_address[i] >> 4) & 0xf];
            g_desc_str[1 + chr_count++] = "0123456789ABCDEF"[tud_network_mac_address[i] & 0xf];
        }
        break;
    default: {
        const char *str;
        if (index >= TU_ARRAY_SIZE(k_string_desc) || !k_string_desc[index]) return NULL;
        str = k_string_desc[index];
        chr_count = strlen(str);
        if (chr_count > 32) chr_count = 32;
        for (unsigned i = 0; i < chr_count; ++i) g_desc_str[1 + i] = (uint8_t)str[i];
        break;
    }
    }

    g_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return g_desc_str;
}

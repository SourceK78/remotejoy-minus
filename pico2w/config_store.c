#include "config_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "btstack_tlv.h"

#define RJM_STORE_MAGIC 0x314D4A52u
#define RJM_STORE_VERSION 1u
#define RJM_STORE_TAG 0x524A4D31u
#define RJM_MAPPING_TAG 0x524A4D32u
#define RJM_MAPPING_MAGIC 0x324D4A52u
#define RJM_P2_SETTING_TAG 0x524A4D34u
#define RJM_P2_SETTING_MAGIC 0x31503252u
#define RJM_DS3_SETTING_TAG 0x524A4D35u
#define RJM_DS3_SETTING_MAGIC 0x31335344u
#define RJM_WIFI_SETTING_TAG 0x524A4D36u
#define RJM_WIFI_SETTING_MAGIC 0x31494657u

struct StoredConfig {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct RjmPortalSlot slots[2];
    uint32_t checksum;
};

struct StoredMapping {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct RjmConfig config;
    uint32_t checksum;
};

struct StoredP2Setting {
    uint32_t magic;
    uint8_t enabled;
    uint8_t reserved[3];
    uint32_t checksum;
};

struct StoredDs3Setting {
    uint32_t magic;
    uint8_t enabled;
    uint8_t reserved[3];
    uint32_t checksum;
};

struct StoredWifiSetting {
    uint32_t magic;
    char password[64];
    uint32_t checksum;
};

static uint32_t checksum(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < length; ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}

bool rjm_config_store_load_slots(struct RjmPortalSlot slots[2])
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredConfig stored;
    btstack_tlv_get_instance(&impl, &context);
    if (!impl || !context || impl->get_tag(context, RJM_STORE_TAG, (uint8_t *)&stored, sizeof(stored)) != sizeof(stored))
        return false;
    if (stored.magic != RJM_STORE_MAGIC || stored.version != RJM_STORE_VERSION ||
        stored.size != sizeof(stored) ||
        stored.checksum != checksum(&stored, offsetof(struct StoredConfig, checksum)))
        return false;
    memcpy(slots, stored.slots, sizeof(stored.slots));
    for (int i = 0; i < 2; ++i) slots[i].connected = false;
    return true;
}

bool rjm_config_store_save_slots(const struct RjmPortalSlot slots[2])
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredConfig stored = {
        .magic = RJM_STORE_MAGIC,
        .version = RJM_STORE_VERSION,
        .size = sizeof(struct StoredConfig),
    };
    memcpy(stored.slots, slots, sizeof(stored.slots));
    for (int i = 0; i < 2; ++i) stored.slots[i].connected = false;
    stored.checksum = checksum(&stored, offsetof(struct StoredConfig, checksum));
    btstack_tlv_get_instance(&impl, &context);
    return impl && context && impl->store_tag(context, RJM_STORE_TAG,
                                               (const uint8_t *)&stored, sizeof(stored)) == 0;
}

bool rjm_config_store_load_mapping(struct RjmConfig *config)
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredMapping stored;
    btstack_tlv_get_instance(&impl, &context);
    if (!impl || !context || impl->get_tag(context, RJM_MAPPING_TAG,
            (uint8_t *)&stored, sizeof(stored)) != sizeof(stored)) return false;
    if (stored.magic != RJM_MAPPING_MAGIC || stored.version != RJM_CONFIG_VERSION ||
        stored.size != sizeof(stored) ||
        stored.checksum != checksum(&stored, offsetof(struct StoredMapping, checksum)) ||
        !rjm_config_validate(&stored.config)) return false;
    *config = stored.config;
    return true;
}

bool rjm_config_store_save_mapping(const struct RjmConfig *config)
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredMapping stored = {
        .magic = RJM_MAPPING_MAGIC,
        .version = RJM_CONFIG_VERSION,
        .size = sizeof(struct StoredMapping),
        .config = *config,
    };
    if (!rjm_config_validate(config)) return false;
    stored.checksum = checksum(&stored, offsetof(struct StoredMapping, checksum));
    btstack_tlv_get_instance(&impl, &context);
    return impl && context && impl->store_tag(context, RJM_MAPPING_TAG,
        (const uint8_t *)&stored, sizeof(stored)) == 0;
}

bool rjm_config_store_load_p2_enabled(bool *enabled)
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredP2Setting stored;
    if (!enabled) return false;
    btstack_tlv_get_instance(&impl, &context);
    if (!impl || !context || impl->get_tag(context, RJM_P2_SETTING_TAG,
            (uint8_t *)&stored, sizeof(stored)) != sizeof(stored)) return false;
    if (stored.magic != RJM_P2_SETTING_MAGIC || stored.enabled > 1 ||
        stored.checksum != checksum(&stored, offsetof(struct StoredP2Setting, checksum))) return false;
    *enabled = stored.enabled != 0;
    return true;
}

bool rjm_config_store_save_p2_enabled(bool enabled)
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredP2Setting stored = {
        .magic = RJM_P2_SETTING_MAGIC,
        .enabled = enabled ? 1 : 0,
    };
    stored.checksum = checksum(&stored, offsetof(struct StoredP2Setting, checksum));
    btstack_tlv_get_instance(&impl, &context);
    return impl && context && impl->store_tag(context, RJM_P2_SETTING_TAG,
        (const uint8_t *)&stored, sizeof(stored)) == 0;
}

bool rjm_config_store_load_ds3_mode(bool *enabled)
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredDs3Setting stored;
    if (!enabled) return false;
    btstack_tlv_get_instance(&impl, &context);
    if (!impl || !context || impl->get_tag(context, RJM_DS3_SETTING_TAG,
            (uint8_t *)&stored, sizeof(stored)) != sizeof(stored)) return false;
    if (stored.magic != RJM_DS3_SETTING_MAGIC || stored.enabled > 1 ||
        stored.checksum != checksum(&stored, offsetof(struct StoredDs3Setting, checksum))) return false;
    *enabled = stored.enabled != 0;
    return true;
}

bool rjm_config_store_save_ds3_mode(bool enabled)
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredDs3Setting stored = {
        .magic = RJM_DS3_SETTING_MAGIC,
        .enabled = enabled ? 1 : 0,
    };
    stored.checksum = checksum(&stored, offsetof(struct StoredDs3Setting, checksum));
    btstack_tlv_get_instance(&impl, &context);
    return impl && context && impl->store_tag(context, RJM_DS3_SETTING_TAG,
        (const uint8_t *)&stored, sizeof(stored)) == 0;
}

bool rjm_config_store_load_wifi_password(char password[64])
{
    const btstack_tlv_t *impl;
    void *context;
    struct StoredWifiSetting stored;
    if (!password) return false;
    btstack_tlv_get_instance(&impl, &context);
    if (!impl || !context || impl->get_tag(context, RJM_WIFI_SETTING_TAG,
            (uint8_t *)&stored, sizeof(stored)) != sizeof(stored)) return false;
    if (stored.magic != RJM_WIFI_SETTING_MAGIC ||
        stored.checksum != checksum(&stored, offsetof(struct StoredWifiSetting, checksum)) ||
        !memchr(stored.password, 0, sizeof(stored.password))) return false;
    memcpy(password, stored.password, 64);
    return true;
}

bool rjm_config_store_save_wifi_password(const char *password)
{
    const btstack_tlv_t *impl;
    void *context;
    size_t password_len = password ? strlen(password) : 0;
    if (password_len < 8 || password_len > 63) return false;
    for (size_t i = 0; i < password_len; ++i)
        if ((uint8_t)password[i] < 0x20 || (uint8_t)password[i] > 0x7e) return false;
    struct StoredWifiSetting stored = {.magic = RJM_WIFI_SETTING_MAGIC};
    memcpy(stored.password, password, password_len + 1);
    stored.checksum = checksum(&stored, offsetof(struct StoredWifiSetting, checksum));
    btstack_tlv_get_instance(&impl, &context);
    return impl && context && impl->store_tag(context, RJM_WIFI_SETTING_TAG,
        (const uint8_t *)&stored, sizeof(stored)) == 0;
}

bool rjm_config_store_reset_wifi(void)
{
    const btstack_tlv_t *impl;
    void *context;
    btstack_tlv_get_instance(&impl, &context);
    if (!impl || !context) return false;
    impl->delete_tag(context, RJM_WIFI_SETTING_TAG);
    return true;
}

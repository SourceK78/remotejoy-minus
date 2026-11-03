#include "mapping_config.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define MAPPING_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define MAPPING_MAGIC 0x5A4D4A52u /* "RJMZ" */
#define MAPPING_STORE_VERSION 1u
#define MAPPING_SAVE_DELAY_MS 2000u

struct StoredMapping {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    struct RjmConfig config;
    uint32_t checksum;
};

struct RjmConfig g_rjm_mapping;

static bool g_save_pending;
static uint32_t g_save_due_ms;

static uint32_t checksum(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < length; ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}

static void map(struct RjmMappingProfile *profile, enum RjmMappableInput input,
                enum RjmOutput output)
{
    profile->output[input] = (uint8_t)output;
}

static void set_name(struct RjmMappingProfile *profile, const char *name)
{
    snprintf(profile->name, sizeof(profile->name), "%s", name);
}

/* The default profiles reproduce the former fixed mapping and its START+R3
 * right-stick modes; START+R3 cycles through them. */
void rjm_mapping_set_defaults(struct RjmConfig *config)
{
    struct RjmMappingProfile *p;

    rjm_config_set_defaults(config);

    p = &config->profile[0];
    p->color = (struct RjmRgb){0, 0, 255};
    /* The left stick has been passed through untouched; keep it that way. */
    p->left_deadzone_percent = 0;
    map(p, RJM_MAP_L2, RJM_OUTPUT_VOLDOWN);
    map(p, RJM_MAP_R2, RJM_OUTPUT_VOLUP);
    map(p, RJM_MAP_L3, RJM_OUTPUT_NOTE);
    map(p, RJM_MAP_R3, RJM_OUTPUT_HOME);
    map(p, RJM_MAP_SYSTEM, RJM_OUTPUT_NONE);

    /* Right stick to D-pad. */
    p = &config->profile[1];
    *p = config->profile[0];
    set_name(p, "Right stick D-pad");
    p->color = (struct RjmRgb){0, 255, 0};
    p->right_deadzone_percent = 40;
    map(p, RJM_MAP_RIGHT_UP, RJM_OUTPUT_UP);
    map(p, RJM_MAP_RIGHT_RIGHT, RJM_OUTPUT_RIGHT);
    map(p, RJM_MAP_RIGHT_DOWN, RJM_OUTPUT_DOWN);
    map(p, RJM_MAP_RIGHT_LEFT, RJM_OUTPUT_LEFT);

    /* Right stick to face buttons, face buttons to D-pad. */
    p = &config->profile[2];
    *p = config->profile[0];
    set_name(p, "Face swap");
    p->color = (struct RjmRgb){255, 0, 255};
    map(p, RJM_MAP_Y, RJM_OUTPUT_UP);
    map(p, RJM_MAP_B, RJM_OUTPUT_RIGHT);
    map(p, RJM_MAP_A, RJM_OUTPUT_DOWN);
    map(p, RJM_MAP_X, RJM_OUTPUT_LEFT);
    map(p, RJM_MAP_RIGHT_UP, RJM_OUTPUT_TRIANGLE);
    map(p, RJM_MAP_RIGHT_RIGHT, RJM_OUTPUT_CIRCLE);
    map(p, RJM_MAP_RIGHT_DOWN, RJM_OUTPUT_CROSS);
    map(p, RJM_MAP_RIGHT_LEFT, RJM_OUTPUT_SQUARE);

    /* Star Soldier vertical play: D-pad rotated. */
    p = &config->profile[3];
    *p = config->profile[0];
    set_name(p, "Star Soldier");
    p->color = (struct RjmRgb){0, 255, 255};
    map(p, RJM_MAP_DPAD_UP, RJM_OUTPUT_RIGHT);
    map(p, RJM_MAP_DPAD_DOWN, RJM_OUTPUT_LEFT);
    map(p, RJM_MAP_DPAD_LEFT, RJM_OUTPUT_UP);
    map(p, RJM_MAP_DPAD_RIGHT, RJM_OUTPUT_DOWN);

    config->profile_count = 4;
    config->active_profile = 0;
}

void rjm_mapping_load(void)
{
    const struct StoredMapping *stored =
        (const struct StoredMapping *)(XIP_BASE + MAPPING_FLASH_OFFSET);

    if (stored->magic == MAPPING_MAGIC && stored->version == MAPPING_STORE_VERSION &&
        stored->size == sizeof(*stored) &&
        stored->checksum == checksum(stored, offsetof(struct StoredMapping, checksum)) &&
        rjm_config_validate(&stored->config)) {
        g_rjm_mapping = stored->config;
        printf("Mapping loaded from flash: %u profiles, active %u\n",
               g_rjm_mapping.profile_count, g_rjm_mapping.active_profile);
        return;
    }
    rjm_mapping_set_defaults(&g_rjm_mapping);
    printf("Mapping not found in flash; using defaults\n");
}

bool rjm_mapping_save(void)
{
    static uint8_t page_buf[(sizeof(struct StoredMapping) + FLASH_PAGE_SIZE - 1) /
                            FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
    struct StoredMapping *stored = (struct StoredMapping *)page_buf;
    uint32_t irq;

    if (!rjm_config_validate(&g_rjm_mapping)) return false;

    memset(page_buf, 0xFF, sizeof(page_buf));
    stored->magic = MAPPING_MAGIC;
    stored->version = MAPPING_STORE_VERSION;
    stored->size = sizeof(*stored);
    stored->config = g_rjm_mapping;
    stored->checksum = checksum(stored, offsetof(struct StoredMapping, checksum));

    /* Single core firmware: masking interrupts keeps flash-resident code from
     * running while XIP is off. */
    irq = save_and_disable_interrupts();
    flash_range_erase(MAPPING_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(MAPPING_FLASH_OFFSET, page_buf, sizeof(page_buf));
    restore_interrupts(irq);

    g_save_pending = false;
    return memcmp((const void *)(XIP_BASE + MAPPING_FLASH_OFFSET), page_buf,
                  sizeof(page_buf)) == 0;
}

const struct RjmMappingProfile *rjm_mapping_active_profile(void)
{
    if (!rjm_config_validate(&g_rjm_mapping)) rjm_mapping_set_defaults(&g_rjm_mapping);
    return &g_rjm_mapping.profile[g_rjm_mapping.active_profile];
}

struct RjmRgb rjm_mapping_active_color(void)
{
    return rjm_mapping_active_profile()->color;
}

void rjm_mapping_next_profile(void)
{
    rjm_mapping_active_profile();
    g_rjm_mapping.active_profile =
        (uint8_t)((g_rjm_mapping.active_profile + 1) % g_rjm_mapping.profile_count);
    printf("Mapping profile %u: %s\n", g_rjm_mapping.active_profile,
           g_rjm_mapping.profile[g_rjm_mapping.active_profile].name);
    g_save_pending = true;
    g_save_due_ms = to_ms_since_boot(get_absolute_time()) + MAPPING_SAVE_DELAY_MS;
}

void rjm_mapping_poll(void)
{
    if (g_save_pending &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - g_save_due_ms) >= 0)
        rjm_mapping_save();
}

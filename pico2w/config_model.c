#include "config_model.h"

#include <string.h>

static const uint32_t k_psp_masks[RJM_PSP_BUTTON_COUNT] = {
    0x000001u, 0x000008u, 0x000010u, 0x000020u,
    0x000040u, 0x000080u, 0x000100u, 0x000200u,
    0x001000u, 0x002000u, 0x004000u, 0x008000u,
    0x010000u, 0x100000u, 0x200000u, 0x800000u,
};

uint32_t rjm_psp_button_mask(enum RjmPspButton button)
{
    return button < RJM_PSP_BUTTON_COUNT ? k_psp_masks[button] : 0;
}

static void map(struct RjmMappingProfile *profile, enum RjmMappableInput input,
                enum RjmOutput output)
{
    profile->output[input] = (uint8_t)output;
}

void rjm_config_set_defaults(struct RjmConfig *config)
{
    struct RjmMappingProfile *profile;
    memset(config, 0, sizeof(*config));
    config->version = RJM_CONFIG_VERSION;
    config->profile_count = 1;
    profile = &config->profile[0];
    memcpy(profile->name, "Default", sizeof("Default"));
    profile->color.blue = 255;
    profile->left_deadzone_percent = 10;
    profile->right_deadzone_percent = 20;
    map(profile, RJM_MAP_A, RJM_OUTPUT_CROSS);
    map(profile, RJM_MAP_B, RJM_OUTPUT_CIRCLE);
    map(profile, RJM_MAP_X, RJM_OUTPUT_SQUARE);
    map(profile, RJM_MAP_Y, RJM_OUTPUT_TRIANGLE);
    map(profile, RJM_MAP_DPAD_UP, RJM_OUTPUT_UP);
    map(profile, RJM_MAP_DPAD_RIGHT, RJM_OUTPUT_RIGHT);
    map(profile, RJM_MAP_DPAD_DOWN, RJM_OUTPUT_DOWN);
    map(profile, RJM_MAP_DPAD_LEFT, RJM_OUTPUT_LEFT);
    map(profile, RJM_MAP_L1, RJM_OUTPUT_LTRIGGER);
    map(profile, RJM_MAP_R1, RJM_OUTPUT_RTRIGGER);
    map(profile, RJM_MAP_SELECT, RJM_OUTPUT_SELECT);
    map(profile, RJM_MAP_START, RJM_OUTPUT_START);
    map(profile, RJM_MAP_SYSTEM, RJM_OUTPUT_HOME);
}

bool rjm_config_validate(const struct RjmConfig *config)
{
    if (!config || config->version != RJM_CONFIG_VERSION || config->profile_count == 0 ||
        config->profile_count > RJM_PROFILE_MAX || config->active_profile >= config->profile_count)
        return false;
    for (uint8_t p = 0; p < config->profile_count; ++p) {
        const struct RjmMappingProfile *profile = &config->profile[p];
        if (!memchr(profile->name, '\0', sizeof(profile->name)) ||
            profile->left_deadzone_percent > 90 || profile->right_deadzone_percent > 90)
            return false;
        for (uint8_t i = 0; i < RJM_MAPPABLE_INPUT_COUNT; ++i)
            if (profile->output[i] >= RJM_OUTPUT_COUNT) return false;
    }
    for (uint8_t i = 0; i < RJM_SLOT_COUNT; ++i)
        if (config->slot[i].controller_id_len > RJM_CONTROLLER_ID_MAX) return false;
    return true;
}

static int16_t apply_deadzone(int16_t value, uint8_t percent)
{
    int32_t v = value;
    int32_t magnitude = v < 0 ? -v : v;
    int32_t threshold = 32767 * percent / 100;
    if (magnitude <= threshold) return 0;
    magnitude = (magnitude - threshold) * 32767 / (32767 - threshold);
    if (magnitude > 32767) magnitude = 32767;
    return (int16_t)(v < 0 ? -magnitude : magnitude);
}

static uint8_t axis_to_u8(int16_t value)
{
    return (uint8_t)(((int32_t)value + 32768) * 255 / 65535);
}

static void apply_output(uint8_t target, struct RjmMappedState *output)
{
    if (target == RJM_OUTPUT_COMBO) output->combo = true;
    else if (target >= RJM_OUTPUT_SELECT && target <= RJM_OUTPUT_NOTE)
        output->buttons |= k_psp_masks[target - 1];
}

void rjm_apply_profile(const struct RjmMappingProfile *profile,
                       const struct RjmNormalizedState *input,
                       struct RjmMappedState *output)
{
    memset(output, 0, sizeof(*output));
    int16_t lx = apply_deadzone(input->axis[RJM_AXIS_LEFT_X], profile->left_deadzone_percent);
    int16_t ly = apply_deadzone(input->axis[RJM_AXIS_LEFT_Y], profile->left_deadzone_percent);
    int16_t rx = apply_deadzone(input->axis[RJM_AXIS_RIGHT_X], profile->right_deadzone_percent);
    int16_t ry = apply_deadzone(input->axis[RJM_AXIS_RIGHT_Y], profile->right_deadzone_percent);
    output->x = axis_to_u8(lx);
    output->y = axis_to_u8(ly);
    for (uint8_t i = RJM_MAP_A; i <= RJM_MAP_MISC; ++i)
        if (input->inputs & (1u << (i + 1))) apply_output(profile->output[i], output);
    if (ry < 0) apply_output(profile->output[RJM_MAP_RIGHT_UP], output);
    if (rx > 0) apply_output(profile->output[RJM_MAP_RIGHT_RIGHT], output);
    if (ry > 0) apply_output(profile->output[RJM_MAP_RIGHT_DOWN], output);
    if (rx < 0) apply_output(profile->output[RJM_MAP_RIGHT_LEFT], output);
}

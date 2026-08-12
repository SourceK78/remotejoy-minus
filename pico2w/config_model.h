#ifndef RJM_CONFIG_MODEL_H
#define RJM_CONFIG_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RJM_CONFIG_VERSION 2u
#define RJM_PROFILE_MAX 8u
#define RJM_PROFILE_NAME_MAX 24u
#define RJM_SLOT_COUNT 2u
#define RJM_PSP_BUTTON_COUNT 16u
#define RJM_MAPPABLE_INPUT_COUNT 22u
#define RJM_CONTROLLER_ID_MAX 24u

/* Controller inputs normalized before applying a profile. */
enum RjmInput {
    RJM_INPUT_NONE = 0,
    RJM_INPUT_A,
    RJM_INPUT_B,
    RJM_INPUT_X,
    RJM_INPUT_Y,
    RJM_INPUT_DPAD_UP,
    RJM_INPUT_DPAD_RIGHT,
    RJM_INPUT_DPAD_DOWN,
    RJM_INPUT_DPAD_LEFT,
    RJM_INPUT_L1,
    RJM_INPUT_R1,
    RJM_INPUT_L2,
    RJM_INPUT_R2,
    RJM_INPUT_L3,
    RJM_INPUT_R3,
    RJM_INPUT_SELECT,
    RJM_INPUT_START,
    RJM_INPUT_SYSTEM,
    RJM_INPUT_MISC,
    RJM_INPUT_COUNT
};

enum RjmPspButton {
    RJM_PSP_SELECT = 0,
    RJM_PSP_START,
    RJM_PSP_UP,
    RJM_PSP_RIGHT,
    RJM_PSP_DOWN,
    RJM_PSP_LEFT,
    RJM_PSP_LTRIGGER,
    RJM_PSP_RTRIGGER,
    RJM_PSP_TRIANGLE,
    RJM_PSP_CIRCLE,
    RJM_PSP_CROSS,
    RJM_PSP_SQUARE,
    RJM_PSP_HOME,
    RJM_PSP_VOLUP,
    RJM_PSP_VOLDOWN,
    RJM_PSP_NOTE
};

/* Values stored for each controller-centric input mapping. */
enum RjmOutput {
    RJM_OUTPUT_NONE = 0,
    RJM_OUTPUT_SELECT,
    RJM_OUTPUT_START,
    RJM_OUTPUT_UP,
    RJM_OUTPUT_RIGHT,
    RJM_OUTPUT_DOWN,
    RJM_OUTPUT_LEFT,
    RJM_OUTPUT_LTRIGGER,
    RJM_OUTPUT_RTRIGGER,
    RJM_OUTPUT_TRIANGLE,
    RJM_OUTPUT_CIRCLE,
    RJM_OUTPUT_CROSS,
    RJM_OUTPUT_SQUARE,
    RJM_OUTPUT_HOME,
    RJM_OUTPUT_VOLUP,
    RJM_OUTPUT_VOLDOWN,
    RJM_OUTPUT_NOTE,
    RJM_OUTPUT_COMBO,
    RJM_OUTPUT_COUNT
};

enum RjmMappableInput {
    RJM_MAP_A = 0, RJM_MAP_B, RJM_MAP_X, RJM_MAP_Y,
    RJM_MAP_DPAD_UP, RJM_MAP_DPAD_RIGHT, RJM_MAP_DPAD_DOWN, RJM_MAP_DPAD_LEFT,
    RJM_MAP_L1, RJM_MAP_R1, RJM_MAP_L2, RJM_MAP_R2, RJM_MAP_L3, RJM_MAP_R3,
    RJM_MAP_SELECT, RJM_MAP_START, RJM_MAP_SYSTEM, RJM_MAP_MISC,
    RJM_MAP_RIGHT_UP, RJM_MAP_RIGHT_RIGHT, RJM_MAP_RIGHT_DOWN, RJM_MAP_RIGHT_LEFT
};

enum RjmAxisSource {
    RJM_AXIS_LEFT_X = 0,
    RJM_AXIS_LEFT_Y,
    RJM_AXIS_RIGHT_X,
    RJM_AXIS_RIGHT_Y
};

struct RjmRgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct RjmMappingProfile {
    char name[RJM_PROFILE_NAME_MAX];
    struct RjmRgb color;
    uint8_t output[RJM_MAPPABLE_INPUT_COUNT];
    uint8_t left_deadzone_percent;
    uint8_t right_deadzone_percent;
};

struct RjmControllerSlot {
    bool assigned;
    uint8_t controller_id_len;
    uint8_t controller_id[RJM_CONTROLLER_ID_MAX];
};

struct RjmConfig {
    uint32_t version;
    uint8_t profile_count;
    uint8_t active_profile;
    struct RjmControllerSlot slot[RJM_SLOT_COUNT];
    struct RjmMappingProfile profile[RJM_PROFILE_MAX];
};

struct RjmNormalizedState {
    uint32_t inputs;
    int16_t axis[4];
};

struct RjmMappedState {
    uint32_t buttons;
    uint8_t x;
    uint8_t y;
    bool combo;
};

void rjm_config_set_defaults(struct RjmConfig *config);
bool rjm_config_validate(const struct RjmConfig *config);
void rjm_apply_profile(const struct RjmMappingProfile *profile,
                       const struct RjmNormalizedState *input,
                       struct RjmMappedState *output);
uint32_t rjm_psp_button_mask(enum RjmPspButton button);

#endif

#include "config_model.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    struct RjmConfig config;
    struct RjmNormalizedState input;
    struct RjmMappedState output;

    rjm_config_set_defaults(&config);
    assert(rjm_config_validate(&config));

    memset(&input, 0, sizeof(input));
    input.inputs = (1u << RJM_INPUT_A) | (1u << RJM_INPUT_DPAD_LEFT);
    input.axis[RJM_AXIS_LEFT_X] = -32768;
    input.axis[RJM_AXIS_LEFT_Y] = 32767;
    rjm_apply_profile(&config.profile[0], &input, &output);
    assert(output.buttons == (rjm_psp_button_mask(RJM_PSP_CROSS) |
                              rjm_psp_button_mask(RJM_PSP_LEFT)));
    assert(output.x == 0 && output.y == 255 && !output.combo);

    /* Controller-centric mappings may point any input at COMBO. */
    config.profile[0].output[RJM_MAP_X] = RJM_OUTPUT_COMBO;
    memset(&input, 0, sizeof(input));
    input.inputs = 1u << RJM_INPUT_X;
    rjm_apply_profile(&config.profile[0], &input, &output);
    assert(output.buttons == 0 && output.combo);

    /* Right-stick directions use the configured dead zone. */
    config.profile[0].output[RJM_MAP_RIGHT_RIGHT] = RJM_OUTPUT_CIRCLE;
    memset(&input, 0, sizeof(input));
    input.axis[RJM_AXIS_RIGHT_X] = 3000;
    rjm_apply_profile(&config.profile[0], &input, &output);
    assert(output.buttons == 0);
    input.axis[RJM_AXIS_RIGHT_X] = 20000;
    rjm_apply_profile(&config.profile[0], &input, &output);
    assert(output.buttons == rjm_psp_button_mask(RJM_PSP_CIRCLE));

    config.profile[0].right_deadzone_percent = 91;
    assert(!rjm_config_validate(&config));
    return 0;
}

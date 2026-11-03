#ifndef RJM_ZERO_MAPPING_CONFIG_H
#define RJM_ZERO_MAPPING_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Profile model shared with the Pico 2 W build so exported JSON is
 * interchangeable between the two. */
#include "../pico2w/config_model.h"

/* Working copy of the mapping profiles. Loaded from flash at boot, edited by
 * the Web portal, and read by the PS2 mapping code. */
extern struct RjmConfig g_rjm_mapping;

void rjm_mapping_set_defaults(struct RjmConfig *config);
void rjm_mapping_load(void);
bool rjm_mapping_save(void);

const struct RjmMappingProfile *rjm_mapping_active_profile(void);
struct RjmRgb rjm_mapping_active_color(void);

/* Advances the active profile. The new selection is written to flash a little
 * later from rjm_mapping_poll(), so cycling through profiles quickly costs a
 * single flash erase. */
void rjm_mapping_next_profile(void);
void rjm_mapping_poll(void);

#endif

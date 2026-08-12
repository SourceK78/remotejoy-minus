#ifndef RJM_CONFIG_PORTAL_H
#define RJM_CONFIG_PORTAL_H

#include <stdbool.h>
#include <stdint.h>
#include "config_model.h"

#define RJM_BT_ADDR_LEN 6

struct RjmPortalSlot {
    bool assigned;
    bool connected;
    uint8_t address[RJM_BT_ADDR_LEN];
    char name[40];
};

bool rjm_portal_start(void);
bool rjm_portal_is_started(void);
bool rjm_portal_reset_wifi(void);
int rjm_portal_pairing_slot(void);
void rjm_portal_complete_pairing(int slot, const uint8_t address[RJM_BT_ADDR_LEN], const char *name);
void rjm_portal_set_connected(const uint8_t address[RJM_BT_ADDR_LEN], bool connected);
void rjm_portal_restore_slots(const struct RjmPortalSlot slots[2]);
void rjm_portal_get_slots(struct RjmPortalSlot slots[2]);
void rjm_portal_complete_unpair(const uint8_t address[RJM_BT_ADDR_LEN]);
void rjm_portal_restore_mapping(const struct RjmConfig *config);
struct RjmRgb rjm_portal_active_color(void);
void rjm_portal_apply_active_mapping(const struct RjmNormalizedState *input,
                                     struct RjmMappedState *output);
void rjm_portal_next_profile(void);
bool rjm_portal_p2_enabled(void);
void rjm_portal_restore_p2_enabled(bool enabled);
bool rjm_portal_ds3_mode(void);
void rjm_portal_restore_ds3_mode(bool enabled);

#endif

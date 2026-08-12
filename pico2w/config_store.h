#ifndef RJM_CONFIG_STORE_H
#define RJM_CONFIG_STORE_H

#include <stdbool.h>
#include "config_portal.h"
#include "config_model.h"

bool rjm_config_store_load_slots(struct RjmPortalSlot slots[2]);
bool rjm_config_store_save_slots(const struct RjmPortalSlot slots[2]);
bool rjm_config_store_load_mapping(struct RjmConfig *config);
bool rjm_config_store_save_mapping(const struct RjmConfig *config);
bool rjm_config_store_load_p2_enabled(bool *enabled);
bool rjm_config_store_save_p2_enabled(bool enabled);
bool rjm_config_store_load_ds3_mode(bool *enabled);
bool rjm_config_store_save_ds3_mode(bool enabled);
bool rjm_config_store_load_wifi_password(char password[64]);
bool rjm_config_store_save_wifi_password(const char *password);
bool rjm_config_store_reset_wifi(void);

#endif

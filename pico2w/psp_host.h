#ifndef RJM_PSP_HOST_H
#define RJM_PSP_HOST_H

#include <stdbool.h>
#include "config_model.h"

void rjm_psp_host_init(void);
void rjm_psp_host_set_state(int player, const struct RjmMappedState *state, bool connected);
void rjm_psp_host_set_enabled(bool enabled);
const char *rjm_psp_host_status(void);
bool rjm_psp_host_is_connected(void);

#endif

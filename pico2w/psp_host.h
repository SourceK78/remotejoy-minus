#ifndef RJM_PSP_HOST_H
#define RJM_PSP_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include "config_model.h"

void rjm_psp_host_init(void);
void rjm_psp_host_set_state(int player, const struct RjmMappedState *state, bool connected);
void rjm_psp_host_set_enabled(bool enabled);
/* Absolute aim (0..65535 per axis) for title-specific POPS aim injection.
 * active = false tells the PSP to stop injecting; offscreen = true reports
 * that the controller points past a screen edge. */
void rjm_psp_host_set_aim(bool active, uint16_t x, uint16_t y, bool offscreen);
const char *rjm_psp_host_status(void);
bool rjm_psp_host_is_connected(void);

#endif

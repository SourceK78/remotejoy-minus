#ifndef RJM_CONTROLLER_PLATFORM_H
#define RJM_CONTROLLER_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

bool rjm_controller_request_unpair(int slot);
void rjm_controller_start_pairing(void);
void rjm_controller_update_scan_state(void);
void rjm_controller_prepare_reboot(void);
void rjm_controller_set_pops_context(bool is_pops);
void rjm_controller_get_local_address(uint8_t address[6]);

#endif

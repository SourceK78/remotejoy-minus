#ifndef RJM_BLUEPAD_PLATFORM_H
#define RJM_BLUEPAD_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

bool rjm_bluepad_request_unpair(int slot);
void rjm_bluepad_start_pairing(void);
void rjm_bluepad_update_scan_state(void);
void rjm_bluepad_prepare_reboot(void);
void rjm_bluepad_set_pops_context(bool is_pops);
void rjm_bluepad_get_diagnostics(uint8_t *auth_status, uint16_t *auth_handle,
                                 uint8_t *disconnect_reason, uint16_t *disconnect_handle,
                                 const char **disconnect_origin, uint32_t input_reports[2]);

#endif

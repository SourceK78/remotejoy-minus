#ifndef RJM_ZERO_CONFIG_PORTAL_H
#define RJM_ZERO_CONFIG_PORTAL_H

#include <stdint.h>

typedef void (*rjm_portal_led_fn)(uint8_t red, uint8_t green, uint8_t blue);

/* Runs the Web configuration mode: the native USB port becomes a CDC-NCM
 * network device and http://192.168.7.1/ serves the mapping editor. Never
 * returns; "Finish setup" saves and reboots into normal mode. */
void rjm_config_portal_run(rjm_portal_led_fn set_led) __attribute__((noreturn));

#endif

#include <string.h>

#include <pico/stdlib.h>
#include <hardware/watchdog.h>

#include "bt/bthid/bthid.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/services/players/feedback.h"
#include "core/services/storage/flash.h"

void rjm_joypad_input_event(const input_event_t *event);
void rjm_joypad_device_disconnected(uint8_t conn_index);

void router_submit_input(const input_event_t *event)
{
    rjm_joypad_input_event(event);
}

void router_device_disconnected(uint8_t dev_addr, int8_t instance)
{
    (void)instance;
    rjm_joypad_device_disconnected(dev_addr);
}

static feedback_state_t feedback_states[BTHID_MAX_DEVICES];
const uint8_t PLAYER_LEDS[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x09,
                                 0x0a, 0x0c, 0x0d, 0x0e, 0x0f};

int find_player_index(int dev_addr, int instance)
{
    (void)instance;
    bthid_device_t *device = bthid_get_device((uint8_t)dev_addr);
    return device && device->player_index != 0xff ? device->player_index : -1;
}

void remove_players_by_address(int dev_addr, int instance)
{
    (void)instance;
    bthid_device_t *device = bthid_get_device((uint8_t)dev_addr);
    if (device) device->player_index = 0xff;
}

feedback_state_t *feedback_get_state(uint8_t player_index)
{
    return player_index < BTHID_MAX_DEVICES ? &feedback_states[player_index] : NULL;
}

void feedback_clear_dirty(uint8_t player_index)
{
    if (player_index >= BTHID_MAX_DEVICES) return;
    feedback_states[player_index].rumble_dirty = false;
    feedback_states[player_index].led_dirty = false;
    feedback_states[player_index].triggers_dirty = false;
}

void flash_on_bt_disconnect(void) {}

uint32_t platform_time_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

void platform_reboot(void)
{
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

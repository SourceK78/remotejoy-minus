#include "runtime_ui.h"

#include <stdbool.h>
#include <stdint.h>

#include "btstack_run_loop.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "config_portal.h"
#include "psp_host.h"

#define LED_PLAYER_1 18
#define LED_PLAYER_2 19
#define LED_RGB_RED 20
#define LED_RGB_GREEN 21
#define LED_RGB_BLUE 22
#define UI_TICK_MS 50
#define BOOTSEL_HOLD_TICKS 30
#define BOOTSEL_WIFI_RESET_TICKS 120
#define FAST_BLINK_TICKS 3
#define PLAYER_LED_FULL 255
#define PLAYER_LED_WAITING 51

static btstack_timer_source_t g_ui_timer;
static uint32_t g_tick;
static uint8_t g_bootsel_ticks;

static bool __no_inline_not_in_flash_func(read_bootsel)(void)
{
    const uint cs_pin_index = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) { }
#if PICO_RP2040
    const uint32_t cs_bit = 1u << 1;
#else
    const uint32_t cs_bit = SIO_GPIO_HI_IN_QSPI_CSN_BITS;
#endif
    bool pressed = !(sio_hw->gpio_hi_in & cs_bit);
    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

static bool blink(uint32_t ticks) { return ((g_tick / ticks) & 1u) == 0; }

static void update_player_leds(void)
{
    struct RjmPortalSlot slots[2];
    int pairing = rjm_portal_pairing_slot();
    rjm_portal_get_slots(slots);
    for (int i = 0; i < 2; ++i) {
        uint8_t level = 0;
        if (pairing == i) level = blink(FAST_BLINK_TICKS) ? PLAYER_LED_FULL : 0;
        else if (slots[i].connected) level = PLAYER_LED_FULL;
        else if (slots[i].assigned) level = PLAYER_LED_WAITING;
        pwm_set_gpio_level(i == 0 ? LED_PLAYER_1 : LED_PLAYER_2, level);
    }
}

static void update_rgb_led(void)
{
    bool config_mode = rjm_portal_is_started();
    struct RjmRgb color = rjm_portal_active_color();
    if (config_mode) {
        /* Two-second triangular breathing cycle, red only. */
        uint32_t phase = g_tick % 40u;
        uint8_t level = (uint8_t)((phase <= 20u ? phase : 40u - phase) * 255u / 20u);
        pwm_set_gpio_level(LED_RGB_RED, level);
        pwm_set_gpio_level(LED_RGB_GREEN, 0);
        pwm_set_gpio_level(LED_RGB_BLUE, 0);
    } else if (!rjm_psp_host_is_connected()) {
        pwm_set_gpio_level(LED_RGB_RED, 255);
        pwm_set_gpio_level(LED_RGB_GREEN, 0);
        pwm_set_gpio_level(LED_RGB_BLUE, 0);
    } else {
        pwm_set_gpio_level(LED_RGB_RED, color.red);
        pwm_set_gpio_level(LED_RGB_GREEN, color.green);
        pwm_set_gpio_level(LED_RGB_BLUE, color.blue);
    }
}

static void ui_timer_handler(btstack_timer_source_t *timer)
{
    ++g_tick;
    if (read_bootsel()) {
        if (g_bootsel_ticks < BOOTSEL_WIFI_RESET_TICKS) ++g_bootsel_ticks;
        if (g_bootsel_ticks == BOOTSEL_HOLD_TICKS && !rjm_portal_is_started()) rjm_portal_start();
        if (g_bootsel_ticks == BOOTSEL_WIFI_RESET_TICKS && rjm_portal_reset_wifi())
            watchdog_reboot(0, 0, 100);
    } else {
        g_bootsel_ticks = 0;
    }
    update_player_leds();
    update_rgb_led();
    btstack_run_loop_set_timer(timer, UI_TICK_MS);
    btstack_run_loop_add_timer(timer);
}

void rjm_runtime_ui_init(void)
{
    const uint8_t player_pins[] = {LED_PLAYER_1, LED_PLAYER_2};
    const uint8_t rgb_pins[] = {LED_RGB_RED, LED_RGB_GREEN, LED_RGB_BLUE};
    for (unsigned i = 0; i < sizeof(player_pins); ++i) gpio_set_function(player_pins[i], GPIO_FUNC_PWM);
    for (unsigned i = 0; i < sizeof(rgb_pins); ++i) gpio_set_function(rgb_pins[i], GPIO_FUNC_PWM);
    pwm_config pwm = pwm_get_default_config();
    pwm_config_set_wrap(&pwm, 255);
    pwm_init(pwm_gpio_to_slice_num(LED_PLAYER_1), &pwm, true);
    pwm_init(pwm_gpio_to_slice_num(LED_RGB_RED), &pwm, true);
    pwm_init(pwm_gpio_to_slice_num(LED_RGB_BLUE), &pwm, true);
    update_rgb_led();
    btstack_run_loop_set_timer_handler(&g_ui_timer, ui_timer_handler);
    btstack_run_loop_set_timer(&g_ui_timer, UI_TICK_MS);
    btstack_run_loop_add_timer(&g_ui_timer);
}

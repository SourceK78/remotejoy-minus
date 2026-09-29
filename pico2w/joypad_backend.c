#include "joypad_backend.h"

#include <string.h>

#include <btstack.h>

#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/vendors/sony/ds3_bt.h"
#include "bt/bthid/devices/vendors/sony/ds4_bt.h"
#include "bt/bthid/devices/vendors/sony/ds5_bt.h"
#include "bt/btstack/btstack_host.h"
#include "bt/transport/bt_transport.h"
#include "controller_platform.h"
#include "config_model.h"
#include "config_portal.h"
#include "config_store.h"
#include "core/buttons.h"
#include "core/input_event.h"
#include "core/services/players/feedback.h"
#include "gyro_aim.h"
#include "hardware/timer.h"
#include "psp_host.h"
#include "runtime_ui.h"

extern const bt_transport_t bt_transport_cyw43;

static bool g_reboot_pending;
static bool g_pops_context;
static struct RjmNormalizedState g_input_state[2];
static bool g_input_connected[2];
static bool g_combo_chord[2];
static btstack_packet_callback_registration_t g_hci_callback;
static btstack_timer_source_t g_startup_timer;
static uint32_t g_ds3_enable_retry_ms[BTHID_MAX_DEVICES];
static hci_con_handle_t g_throttled_sony_handle = HCI_CON_HANDLE_INVALID;

static void assign_player(bthid_device_t *device, int player)
{
    if (!device || player < 0 || player >= 2) return;
    if (device->player_index == (uint8_t)player) return;

    device->player_index = (uint8_t)player;
    if (device->driver == &ds3_bt_driver) {
        /* A newly paired DS3 can finish its activation before the Web slot is
         * committed. Explicitly dirty the final player LED when assignment
         * becomes known; otherwise player 1 can retain the driver's fallback
         * LED cache and never receive a post-pairing output report. */
        feedback_state_t *feedback = feedback_get_state((uint8_t)player);
        if (feedback) {
            feedback->led.pattern = (uint8_t)(1u << player);
            feedback->led_dirty = true;
        }
    }
}

/* Gyro aim is produced for player 1 only and only inside POPS, where the PSP
 * plugin presents it to the game as a GunCon. */
#define GYRO_AIM_PLAYER 0

static enum RjmGyroKind gyro_kind_for_device(const bthid_device_t *device)
{
    if (device->driver == &ds4_bt_driver || device->driver == &ds5_bt_driver)
        return RJM_GYRO_KIND_PLAYSTATION;
    /* DS3 reports a single yaw axis only; other drivers carry no motion. */
    return RJM_GYRO_KIND_NONE;
}

static void release_gyro_aim(void)
{
    rjm_gyro_aim_reset(GYRO_AIM_PLAYER);
    rjm_psp_host_set_aim(false, 0, 0, false);
}

static void update_gyro_aim(int player, const bthid_device_t *device,
                            const input_event_t *event, uint32_t inputs)
{
    if (player != GYRO_AIM_PLAYER) return;

    struct RjmGyroSample sample = {
        .kind = event->has_motion ? gyro_kind_for_device(device) : RJM_GYRO_KIND_NONE,
        .gyro_range_dps = event->gyro_range,
        .accel_range_mg = event->accel_range,
        .time_us = time_us_32(),
        .recenter = (inputs & ((1u << RJM_INPUT_L3) | (1u << RJM_INPUT_R3))) != 0,
        .span_deg = rjm_portal_gyro_span(),
    };
    uint16_t x, y;
    bool offscreen = false;

    /* Gyro aim drives the PSP-side GunCon, so it is sent only inside POPS and
     * only while the active profile has GunCon mode on. */
    if (!g_pops_context || !rjm_portal_guncon_enabled() || sample.kind == RJM_GYRO_KIND_NONE) {
        release_gyro_aim();
        return;
    }
    memcpy(sample.gyro, event->gyro, sizeof(sample.gyro));
    memcpy(sample.accel, event->accel, sizeof(sample.accel));
    if (rjm_gyro_aim_update(player, &sample, &x, &y, &offscreen))
        rjm_psp_host_set_aim(true, x, y, offscreen);
    else
        release_gyro_aim();
}

static bool p2_runtime_enabled(void)
{
    return g_pops_context && rjm_portal_p2_enabled();
}

static int player_for_address(const uint8_t address[6])
{
    struct RjmPortalSlot slots[2];
    rjm_portal_get_slots(slots);
    for (int i = 0; i < 2; ++i)
        if (slots[i].assigned && memcmp(slots[i].address, address, 6) == 0) return i;
    return -1;
}

static void complete_pairing_replacing(int slot, const uint8_t address[6], const char *name)
{
    struct RjmPortalSlot slots[2];
    rjm_portal_get_slots(slots);
    if (slot >= 0 && slot < 2 && slots[slot].assigned &&
        memcmp(slots[slot].address, address, 6) != 0)
        btstack_host_forget_device(slots[slot].address);
    rjm_portal_complete_pairing(slot, address, name);
}

static void remap_players(void)
{
    for (int i = 0; i < 2; ++i) {
        struct RjmMappedState mapped;
        if (g_input_connected[i] && (i == 0 || p2_runtime_enabled())) {
            rjm_portal_apply_active_mapping(&g_input_state[i], &mapped);
            if (g_combo_chord[i]) mapped.buttons &= ~rjm_psp_button_mask(RJM_PSP_START);
            rjm_psp_host_set_state(i, &mapped, true);
        } else {
            rjm_psp_host_set_state(i, NULL, false);
        }
    }
}

static void update_scan_state(void)
{
    struct RjmPortalSlot slots[2];
    bool needs_scan = rjm_portal_pairing_slot() >= 0;
    rjm_portal_get_slots(slots);
    for (int i = 0; i < 2 && !needs_scan; ++i) {
        if (i == 1 && !p2_runtime_enabled()) continue;
        if (slots[i].assigned && !slots[i].connected) needs_scan = true;
    }
    if (g_reboot_pending || !needs_scan) btstack_host_stop_scan();
    else btstack_host_start_scan();
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_CONNECTION_REQUEST: {
            /* SixaxisPairTool writes only the host address into a DS3. A key
             * left in Pico flash by an older firmware can therefore look like
             * a reconnect key even though the controller no longer has the
             * matching state. During an explicit portal pairing operation the
             * incoming address is, by definition, being paired afresh. Drop
             * the Classic key before joypad-os decides whether to request its
             * normal Level 2 reconnect authentication. */
            if (rjm_portal_pairing_slot() >= 0) {
                bd_addr_t address;
                hci_event_connection_request_get_bd_addr(packet, address);
                gap_drop_link_key_for_bd_addr(address);
            }
            break;
        }
        default:
            break;
    }
}

static void startup_timer_handler(btstack_timer_source_t *timer)
{
    (void)timer;
    update_scan_state();
}

bool rjm_joypad_backend_init(void)
{
    struct RjmPortalSlot slots[2] = {0};
    struct RjmConfig mapping;
    bool p2_enabled = true;
    bool ds3_mode = false;
    uint8_t gyro_span = RJM_GYRO_SPAN_DEFAULT_DEG;
    uint8_t guncon_mask = 0;

    bt_init(&bt_transport_cyw43);
    /* HCI power-on completes asynchronously. The host handlers being ready is
     * the synchronous initialization boundary; bt_is_ready() becomes true
     * later when BTstack reports HCI_STATE_WORKING. */
    if (!btstack_host_is_initialized()) return false;

    if (rjm_config_store_load_slots(slots)) rjm_portal_restore_slots(slots);
    bool mapping_loaded = rjm_config_store_load_mapping(&mapping);
    if (!mapping_loaded) rjm_config_set_defaults(&mapping);
    rjm_config_store_load_p2_enabled(&p2_enabled);
    rjm_config_store_load_ds3_mode(&ds3_mode);
    rjm_config_store_load_gyro_span(&gyro_span);
    rjm_portal_restore_gyro_span(gyro_span);
    rjm_portal_restore_p2_enabled(p2_enabled);
    rjm_portal_restore_ds3_mode(ds3_mode);
    rjm_portal_restore_mapping(&mapping);
    /* The GunCon bits belong to the stored profiles; drop them with them. */
    if (mapping_loaded) rjm_config_store_load_guncon_mask(&guncon_mask);
    rjm_portal_restore_guncon_mask(guncon_mask);

    rjm_runtime_ui_init();
    rjm_psp_host_init();
    g_hci_callback.callback = hci_packet_handler;
    hci_add_event_handler(&g_hci_callback);
    btstack_run_loop_set_timer_handler(&g_startup_timer, startup_timer_handler);
    btstack_run_loop_set_timer(&g_startup_timer, 750);
    btstack_run_loop_add_timer(&g_startup_timer);
    return true;
}

void rjm_joypad_backend_task(void)
{
    int pairing_slot = rjm_portal_pairing_slot();
    bool ds3_connected = false;
    hci_con_handle_t high_rate_sony_handle = HCI_CON_HANDLE_INVALID;

    /* JoypadOS creates the BTHID device when its HID channels are ready. DS3
     * input reporting is enabled only after a feature-report handshake, so it
     * may not produce an input event immediately. Claim a newly ready,
     * unregistered device here instead of waiting for that first event. */
    for (uint8_t conn_index = 0; conn_index < BTHID_MAX_DEVICES; ++conn_index) {
        bthid_device_t *device = bthid_get_device(conn_index);
        if (!device || !device->active) continue;

        int player = player_for_address(device->bd_addr);
        if (pairing_slot >= 0 && player < 0) {
            complete_pairing_replacing(pairing_slot, device->bd_addr,
                                       device->name[0] ? device->name : "Controller");
            player = player_for_address(device->bd_addr);
            pairing_slot = rjm_portal_pairing_slot();
            update_scan_state();
        }
        if (player >= 0) {
            assign_player(device, player);

            /* BTHID becomes active only after the HID channels and device
             * driver are ready. Use that transport state for the portal/LED
             * connection indication. Some event-driven controllers emit no
             * input report until a button changes, so waiting for the first
             * report leaves a successful new pairing shown as disconnected. */
            struct RjmPortalSlot slots[2];
            rjm_portal_get_slots(slots);
            if (!slots[player].connected) {
                rjm_portal_set_connected(device->bd_addr, true);
                update_scan_state();
            }
        }
        if (player >= 0 && device->driver == &ds3_bt_driver) ds3_connected = true;
        if (player >= 0 &&
            (device->driver == &ds4_bt_driver || device->driver == &ds5_bt_driver))
            high_rate_sony_handle = btstack_classic_get_acl_handle(conn_index);

        /* JoypadOS sends DS3's F4 enable command once from its activation
         * state machine. HID Host SET_REPORT is asynchronous and can still be
         * busy immediately after channel setup; a rejected first send leaves
         * the pad's player LED active but input streaming disabled forever.
         * Retry only while this registered DS3 has produced no input. */
        if (player >= 0 && !g_input_connected[player] &&
            device->driver == &ds3_bt_driver) {
            uint32_t now = btstack_run_loop_get_time_ms();
            if (now - g_ds3_enable_retry_ms[conn_index] >= 500) {
                static const uint8_t enable_data[] = {0x42, 0x03, 0x00, 0x00};
                btstack_classic_send_set_report_type(conn_index,
                                                      3 /* FEATURE */, 0xf4,
                                                      enable_data,
                                                      sizeof(enable_data));
                g_ds3_enable_retry_ms[conn_index] = now;
            }
        }
    }

    hci_con_handle_t desired_handle =
        ds3_connected ? high_rate_sony_handle : HCI_CON_HANDLE_INVALID;
    if (desired_handle != g_throttled_sony_handle) {
        if (g_throttled_sony_handle != HCI_CON_HANDLE_INVALID)
            gap_sniff_mode_exit(g_throttled_sony_handle);
        g_throttled_sony_handle = HCI_CON_HANDLE_INVALID;
        if (desired_handle != HCI_CON_HANDLE_INVALID &&
            gap_sniff_mode_enter(desired_handle, 32, 32, 4, 1) == ERROR_CODE_SUCCESS)
            g_throttled_sony_handle = desired_handle;
    }
}

void rjm_joypad_input_event(const input_event_t *event)
{
    if (!event || (event->transport != INPUT_TRANSPORT_BT_CLASSIC &&
                   event->transport != INPUT_TRANSPORT_BT_BLE)) return;

    bthid_device_t *device = bthid_get_device(event->dev_addr);
    if (!device) return;

    int pairing_slot = rjm_portal_pairing_slot();
    int player = player_for_address(device->bd_addr);
    /* Existing controllers continue to emit reports while another slot is in
     * pairing mode. Only an address that is not already assigned may complete
     * the pending pairing; otherwise 1P's periodic report can steal the 2P
     * slot before the newly paired controller sends its first report. */
    if (pairing_slot >= 0 && player < 0) {
        complete_pairing_replacing(pairing_slot, device->bd_addr,
                                   device->name[0] ? device->name : "Controller");
        player = player_for_address(device->bd_addr);
    } else if (player < 0) {
        btstack_host_forget_device(device->bd_addr);
        return;
    }
    if (player < 0) return;

    assign_player(device, player);
    struct RjmNormalizedState *state = &g_input_state[player];
    uint32_t inputs = 0;
#define INPUT_IF(mask, input) do { if (event->buttons & (mask)) inputs |= 1u << (input); } while (0)
    INPUT_IF(JP_BUTTON_B1, RJM_INPUT_A);
    INPUT_IF(JP_BUTTON_B2, RJM_INPUT_B);
    INPUT_IF(JP_BUTTON_B3, RJM_INPUT_X);
    INPUT_IF(JP_BUTTON_B4, RJM_INPUT_Y);
    INPUT_IF(JP_BUTTON_DU, RJM_INPUT_DPAD_UP);
    INPUT_IF(JP_BUTTON_DR, RJM_INPUT_DPAD_RIGHT);
    INPUT_IF(JP_BUTTON_DD, RJM_INPUT_DPAD_DOWN);
    INPUT_IF(JP_BUTTON_DL, RJM_INPUT_DPAD_LEFT);
    INPUT_IF(JP_BUTTON_L1, RJM_INPUT_L1);
    INPUT_IF(JP_BUTTON_R1, RJM_INPUT_R1);
    INPUT_IF(JP_BUTTON_L2, RJM_INPUT_L2);
    INPUT_IF(JP_BUTTON_R2, RJM_INPUT_R2);
    INPUT_IF(JP_BUTTON_L3, RJM_INPUT_L3);
    INPUT_IF(JP_BUTTON_R3, RJM_INPUT_R3);
    INPUT_IF(JP_BUTTON_S1, RJM_INPUT_SELECT);
    INPUT_IF(JP_BUTTON_S2, RJM_INPUT_START);
    INPUT_IF(JP_BUTTON_A1, RJM_INPUT_SYSTEM);
    INPUT_IF(JP_BUTTON_A2, RJM_INPUT_MISC);
#undef INPUT_IF
    if (event->analog[ANALOG_L2] > 128) inputs |= 1u << RJM_INPUT_L2;
    if (event->analog[ANALOG_R2] > 128) inputs |= 1u << RJM_INPUT_R2;

    state->inputs = inputs;
    state->axis[RJM_AXIS_LEFT_X] = ((int16_t)event->analog[ANALOG_LX] - 128) * 256;
    state->axis[RJM_AXIS_LEFT_Y] = ((int16_t)event->analog[ANALOG_LY] - 128) * 256;
    state->axis[RJM_AXIS_RIGHT_X] = ((int16_t)event->analog[ANALOG_RX] - 128) * 256;
    state->axis[RJM_AXIS_RIGHT_Y] = ((int16_t)event->analog[ANALOG_RY] - 128) * 256;
    update_gyro_aim(player, device, event, inputs);

    if (!g_input_connected[player]) {
        g_input_connected[player] = true;
        rjm_portal_set_connected(device->bd_addr, true);
        update_scan_state();
    }

    struct RjmMappedState mapped;
    rjm_portal_apply_active_mapping(state, &mapped);
    bool start_pressed = (inputs & (1u << RJM_INPUT_START)) != 0;
    bool chord = start_pressed && mapped.combo;
    if (chord && !g_combo_chord[player]) rjm_portal_next_profile();
    if (chord) g_combo_chord[player] = true;
    else if (!start_pressed) g_combo_chord[player] = false;
    remap_players();
}

void rjm_joypad_device_disconnected(uint8_t conn_index)
{
    bthid_device_t *device = bthid_get_device(conn_index);
    if (!device) return;
    int player = player_for_address(device->bd_addr);
    if (player >= 0) {
        memset(&g_input_state[player], 0, sizeof(g_input_state[player]));
        g_input_connected[player] = false;
        g_combo_chord[player] = false;
        rjm_psp_host_set_state(player, NULL, false);
        if (player == GYRO_AIM_PLAYER) release_gyro_aim();
    }
    rjm_portal_set_connected(device->bd_addr, false);
    update_scan_state();
}

bool rjm_controller_request_unpair(int slot)
{
    struct RjmPortalSlot slots[2];
    if (slot < 0 || slot >= 2) return false;
    rjm_portal_get_slots(slots);
    if (!slots[slot].assigned) return false;
    btstack_host_forget_device(slots[slot].address);
    rjm_portal_complete_unpair(slots[slot].address);
    update_scan_state();
    return true;
}

void rjm_controller_start_pairing(void)
{
    struct RjmPortalSlot slots[2];
    int slot = rjm_portal_pairing_slot();
    rjm_portal_get_slots(slots);
    if (slot >= 0 && slot < 2 && slots[slot].assigned)
        btstack_host_forget_device(slots[slot].address);
    btstack_host_start_scan();
}

void rjm_controller_update_scan_state(void) { update_scan_state(); }

void rjm_controller_prepare_reboot(void)
{
    g_reboot_pending = true;
    btstack_host_stop_scan();
    gap_connectable_control(0);
    btstack_host_disconnect_all_devices();
}

void rjm_controller_set_pops_context(bool is_pops)
{
    g_pops_context = is_pops;
    if (!is_pops) release_gyro_aim();
    remap_players();
    update_scan_state();
}

void rjm_controller_get_local_address(uint8_t address[6])
{
    if (address) gap_local_bd_addr(address);
}

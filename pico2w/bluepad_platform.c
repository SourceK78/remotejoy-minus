#include <string.h>

#include <btstack.h>
#include <uni.h>
#include "bt/uni_bt_bredr.h"
#include "bluepad_platform.h"
#include "config_portal.h"
#include "config_store.h"
#include "runtime_ui.h"
#include "psp_host.h"
#include "controller/uni_gamepad.h"

static btstack_context_callback_registration_t g_unpair_callback;
static btstack_context_callback_registration_t g_scan_update_callback;
static btstack_context_callback_registration_t g_reboot_callback;
static btstack_context_callback_registration_t g_context_callback;
static btstack_timer_source_t g_unpair_timer;
static btstack_timer_source_t g_startup_scan_timer;
static btstack_packet_callback_registration_t g_hci_diag_callback;
static bd_addr_t g_unpair_address;
static bool g_unpair_pending;
static bool g_reboot_pending;
static bool g_pops_context;
static uint8_t g_last_auth_status = 0xff;
static uint8_t g_last_disconnect_reason = 0xff;
static uint16_t g_last_auth_handle;
static uint16_t g_last_disconnect_handle;
static struct RjmNormalizedState g_input_state[2];
static bool g_input_connected[2];
static bool g_combo_chord[2];
static uint32_t g_input_reports[2];
static bool g_pair_key_cleared;
static bd_addr_t g_pair_key_address;
static hci_con_handle_t g_throttled_dualsense_handle = UNI_BT_CONN_HANDLE_INVALID;
static int player_for_address(const uint8_t address[6]);

/* A DualSense can generate substantially more HID traffic than a DS3.  When
   DS3 compatibility mode is selected and both controller types are ready,
   put only the DualSense ACL link in a 20 ms Sniff interval.  This is kept
   deliberately narrow so normal mode and other controllers are unaffected. */
static void update_ds3_dualsense_throttle(uni_hid_device_t *becoming_ready,
                                          uni_hid_device_t *disconnecting)
{
    uni_hid_device_t *ds3 = NULL;
    uni_hid_device_t *dualsense = NULL;

    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        uni_hid_device_t *device = uni_hid_device_get_instance_for_idx(i);
        if (!device || device == disconnecting || uni_hid_device_is_virtual_device(device) ||
            !device->conn.connected ||
            (device != becoming_ready && device->conn.state != UNI_BT_CONN_STATE_DEVICE_READY) ||
            device->conn.handle == UNI_BT_CONN_HANDLE_INVALID)
            continue;
        if (device->controller_type == k_eControllerType_PS3Controller) ds3 = device;
        else if (device->controller_type == k_eControllerType_PS5Controller) dualsense = device;
    }

    bool should_throttle = rjm_portal_ds3_mode() && ds3 && dualsense;
    hci_con_handle_t desired_handle = should_throttle ? dualsense->conn.handle : UNI_BT_CONN_HANDLE_INVALID;
    if (desired_handle == g_throttled_dualsense_handle) return;

    if (g_throttled_dualsense_handle != UNI_BT_CONN_HANDLE_INVALID &&
        (!disconnecting || disconnecting->conn.handle != g_throttled_dualsense_handle))
        gap_sniff_mode_exit(g_throttled_dualsense_handle);
    g_throttled_dualsense_handle = UNI_BT_CONN_HANDLE_INVALID;

    if (desired_handle != UNI_BT_CONN_HANDLE_INVALID) {
        /* Bluetooth interval units are 0.625 ms: 32 = 20 ms. */
        if (gap_sniff_mode_enter(desired_handle, 32, 32, 4, 1) == ERROR_CODE_SUCCESS)
            g_throttled_dualsense_handle = desired_handle;
    }
}

static void clear_pairing_key_once(const uint8_t address[6])
{
    if (rjm_portal_pairing_slot() < 0) return;
    if (g_pair_key_cleared && memcmp(g_pair_key_address, address, 6) == 0) return;
    gap_drop_link_key_for_bd_addr((uint8_t *)address);
    memcpy(g_pair_key_address, address, 6);
    g_pair_key_cleared = true;
}

static bool p2_runtime_enabled(void)
{
    return g_pops_context && rjm_portal_p2_enabled();
}

static void hci_diag_packet_handler(uint8_t packet_type, uint16_t channel,
                                    uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_CONNECTION_REQUEST: {
            /* Explicit portal pairing means that any key for the incoming
               address is stale by definition.  With L2CAP level 0 (required
               by DS3), a DualSense otherwise tries that stale key once and
               authentication ends with 0x05 instead of creating a new key. */
            if (rjm_portal_pairing_slot() >= 0) {
                bd_addr_t address;
                hci_event_connection_request_get_bd_addr(packet, address);
                clear_pairing_key_once(address);
            }
            break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            g_last_auth_status = hci_event_authentication_complete_get_status(packet);
            g_last_auth_handle = hci_event_authentication_complete_get_connection_handle(packet);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            g_last_disconnect_reason = hci_event_disconnection_complete_get_reason(packet);
            g_last_disconnect_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            break;
        default:
            break;
    }
}

void rjm_bluepad_get_diagnostics(uint8_t *auth_status, uint16_t *auth_handle,
                                 uint8_t *disconnect_reason, uint16_t *disconnect_handle,
                                 const char **disconnect_origin, uint32_t input_reports[2])
{
    if (auth_status) *auth_status = g_last_auth_status;
    if (auth_handle) *auth_handle = g_last_auth_handle;
    if (disconnect_reason) *disconnect_reason = g_last_disconnect_reason;
    if (disconnect_handle) *disconnect_handle = g_last_disconnect_handle;
    if (disconnect_origin) *disconnect_origin = "not-recorded";
    if (input_reports) {
        input_reports[0] = g_input_reports[0];
        input_reports[1] = g_input_reports[1];
    }
}

static void update_scan_state_unsafe(void)
{
    if (g_reboot_pending) {
        uni_bt_stop_scanning_unsafe();
        return;
    }
    struct RjmPortalSlot slots[2];
    bool needs_scan = rjm_portal_pairing_slot() >= 0;
    rjm_portal_get_slots(slots);
    for (int i = 0; i < 2 && !needs_scan; ++i) {
        if (i == 1 && !p2_runtime_enabled()) continue;
        if (slots[i].assigned && !slots[i].connected) needs_scan = true;
    }
    if (needs_scan) uni_bt_start_scanning_and_autoconnect_unsafe();
    else uni_bt_stop_scanning_unsafe();
}

static int player_for_address(const uint8_t address[6])
{
    struct RjmPortalSlot slots[2];
    rjm_portal_get_slots(slots);
    for (int i = 0; i < 2; ++i)
        if (slots[i].assigned && memcmp(slots[i].address, address, 6) == 0) return i;
    return -1;
}

static int16_t normalize_axis(int32_t value)
{
    if (value < -512) value = -512;
    if (value > 511) value = 511;
    return (int16_t)(value * 64);
}

static void remap_players(void)
{
    for (int i = 0; i < 2; ++i) {
        struct RjmMappedState mapped;
        if (g_input_connected[i] && (i == 0 || p2_runtime_enabled())) {
            rjm_portal_apply_active_mapping(&g_input_state[i], &mapped);
            if (g_combo_chord[i]) mapped.buttons &= ~rjm_psp_button_mask(RJM_PSP_START);
            rjm_psp_host_set_state(i, &mapped, true);
        } else rjm_psp_host_set_state(i, NULL, false);
    }
}

static void perform_scan_update(void *context)
{
    (void)context;
    update_scan_state_unsafe();
}

static void startup_scan_timer_handler(btstack_timer_source_t *timer)
{
    (void)timer;
    update_scan_state_unsafe();
}

void rjm_bluepad_update_scan_state(void)
{
    g_scan_update_callback.callback = perform_scan_update;
    g_scan_update_callback.context = NULL;
    btstack_run_loop_execute_on_main_thread(&g_scan_update_callback);
}

static void prepare_reboot(void *context)
{
    (void)context;
    g_reboot_pending = true;
    uni_bt_stop_scanning_unsafe();
    /* Stopping inquiry does not stop an already-paired BR/EDR controller from
       paging us.  DualSense retries very quickly after a host disconnect, so
       reject incoming ACL connections until the watchdog reset.  Bluepad32
       enables connectability again during the next normal initialization. */
    gap_connectable_control(0);

    /* Tell controllers that the link is intentionally going away before the
       watchdog resets CYW43.  In particular, some 8BitDo controllers keep the
       old ACL link alive for a while after an abrupt host reset. */
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        uni_hid_device_t *device = uni_hid_device_get_instance_for_idx(i);
        if (device && !uni_hid_device_is_virtual_device(device) && device->conn.connected)
            uni_hid_device_disconnect(device);
    }
}

void rjm_bluepad_prepare_reboot(void)
{
    g_reboot_callback.callback = prepare_reboot;
    g_reboot_callback.context = NULL;
    btstack_run_loop_execute_on_main_thread(&g_reboot_callback);
}

static void apply_pops_context(void *context)
{
    g_pops_context = context != NULL;
    /* Keep an existing 2P Bluetooth link alive outside POPS, but immediately
       release its PSP state and exclude a missing 2P pad from inquiry. */
    remap_players();
    update_scan_state_unsafe();
}

void rjm_bluepad_set_pops_context(bool is_pops)
{
    g_context_callback.callback = apply_pops_context;
    g_context_callback.context = is_pops ? (void *)1 : NULL;
    btstack_run_loop_execute_on_main_thread(&g_context_callback);
}

static void drop_bond_for_address(const uint8_t address[6]);

static void finish_unpair(void)
{
    struct RjmPortalSlot slots[2];
    if (!g_unpair_pending) return;
    btstack_run_loop_remove_timer(&g_unpair_timer);
    drop_bond_for_address(g_unpair_address);
    rjm_portal_complete_unpair(g_unpair_address);
    rjm_portal_get_slots(slots);
    if (!slots[0].assigned && !slots[1].assigned)
        uni_bt_del_keys_unsafe();
    g_unpair_pending = false;
    update_scan_state_unsafe();
}

static void unpair_timeout(btstack_timer_source_t *timer)
{
    (void)timer;
    finish_unpair();
}

static void perform_unpair(void *context)
{
    (void)context;
    uni_bt_stop_scanning_unsafe();
    uni_hid_device_t *device = uni_hid_device_get_instance_for_address(g_unpair_address);
    if (device) {
        uni_hid_device_disconnect(device);
        btstack_run_loop_set_timer_handler(&g_unpair_timer, unpair_timeout);
        btstack_run_loop_set_timer(&g_unpair_timer, 1000);
        btstack_run_loop_add_timer(&g_unpair_timer);
    } else {
        finish_unpair();
    }
}

static void drop_bond_for_address(const uint8_t address[6])
{
    bd_addr_t target_address;
    bd_addr_t entry_address;
    int entry_type;
    memcpy(target_address, address, sizeof(target_address));
    gap_drop_link_key_for_bd_addr(target_address);
    for (int i = 0; i < le_device_db_max_count(); ++i) {
        le_device_db_info(i, &entry_type, entry_address, NULL);
        if (bd_addr_cmp(entry_address, target_address) == 0)
            gap_delete_bonding((bd_addr_type_t)entry_type, entry_address);
    }
}

void rjm_bluepad_start_pairing(void)
{
    struct RjmPortalSlot slots[2];
    int slot = rjm_portal_pairing_slot();
    g_pair_key_cleared = false;
    memset(g_pair_key_address, 0, sizeof(g_pair_key_address));
    /* If this operation replaces/re-pairs an assigned slot, its address is
       already known. Remove the old key before inquiry or paging starts;
       deleting it from a discovery/connection callback can race the
       controller's authentication attempt. */
    rjm_portal_get_slots(slots);
    if (slot >= 0 && slot < 2 && slots[slot].assigned) {
        gap_drop_link_key_for_bd_addr(slots[slot].address);
        memcpy(g_pair_key_address, slots[slot].address, sizeof(g_pair_key_address));
        g_pair_key_cleared = true;
    }
    /* At L2CAP level 0, forcing authentication immediately after ACL setup
       makes a DualSense in pairing mode fail with 0x05. Let its HID channel
       setup drive SSP instead. Normal reconnections keep the explicit level-2
       request. */
    uni_bt_bredr_set_defer_security_request(rjm_portal_ds3_mode());
    uni_bt_start_scanning_and_autoconnect_safe();
}

bool rjm_bluepad_request_unpair(int slot)
{
    struct RjmPortalSlot slots[2];
    if (slot < 0 || slot >= 2 || g_unpair_pending) return false;
    rjm_portal_get_slots(slots);
    if (!slots[slot].assigned) return false;
    memcpy(g_unpair_address, slots[slot].address, sizeof(g_unpair_address));
    g_unpair_pending = true;
    g_unpair_callback.callback = perform_unpair;
    g_unpair_callback.context = NULL;
    btstack_run_loop_execute_on_main_thread(&g_unpair_callback);
    return true;
}

static void platform_init(int argc, const char **argv)
{
    bool ds3_mode = false;
    (void)argc; (void)argv;
    rjm_config_store_load_ds3_mode(&ds3_mode);
    uni_bt_set_gap_security_level(ds3_mode ? 0 : 2);
}

static void on_init_complete(void)
{
    struct RjmPortalSlot slots[2] = {0};
    struct RjmConfig mapping;
    bool p2_enabled = true;
    bool ds3_mode = false;
    if (rjm_config_store_load_slots(slots)) rjm_portal_restore_slots(slots);
    if (!rjm_config_store_load_mapping(&mapping)) rjm_config_set_defaults(&mapping);
    rjm_config_store_load_p2_enabled(&p2_enabled);
    rjm_config_store_load_ds3_mode(&ds3_mode);
    rjm_portal_restore_p2_enabled(p2_enabled);
    rjm_portal_restore_ds3_mode(ds3_mode);
    rjm_portal_restore_mapping(&mapping);
    rjm_runtime_ui_init();
    rjm_psp_host_init();
    g_hci_diag_callback.callback = hci_diag_packet_handler;
    hci_add_event_handler(&g_hci_diag_callback);
    /* Keep Bluepad32 bond keys: they provide persistent reconnection. */
    uni_bt_list_keys_unsafe();
    /* Give controllers time to finish processing the previous host disconnect
       before starting BR/EDR inquiry again after a configuration-mode reboot. */
    btstack_run_loop_set_timer_handler(&g_startup_scan_timer, startup_scan_timer_handler);
    btstack_run_loop_set_timer(&g_startup_scan_timer, 750);
    btstack_run_loop_add_timer(&g_startup_scan_timer);
}

static uni_error_t on_discovered(bd_addr_t address, const char *name, uint16_t cod, uint8_t rssi)
{
    (void)name; (void)cod; (void)rssi;
    /* Scanning may make Pico initiate the ACL connection, in which case no
       incoming HCI connection-request event is emitted. Clear the stale key
       here as well while explicit pairing is active. */
    clear_pairing_key_once(address);
    return UNI_ERROR_SUCCESS;
}

static void on_connected(uni_hid_device_t *device)
{
    /* A page request might have completed just before connectability was
       disabled.  Do not let that new link get cut abruptly by the pending
       watchdog reset. */
    if (g_reboot_pending) {
        uni_hid_device_disconnect(device);
    }
}

static void on_disconnected(uni_hid_device_t *device)
{
    update_ds3_dualsense_throttle(NULL, device);
    int player = player_for_address(device->conn.btaddr);
    bool completes_unpair = g_unpair_pending &&
        memcmp(device->conn.btaddr, g_unpair_address, sizeof(g_unpair_address)) == 0;
    if (player >= 0) {
        memset(&g_input_state[player], 0, sizeof(g_input_state[player]));
        g_input_connected[player] = false;
        g_combo_chord[player] = false;
        rjm_psp_host_set_state(player, NULL, false);
    }
    rjm_portal_set_connected(device->conn.btaddr, false);
    if (completes_unpair) finish_unpair();
    else update_scan_state_unsafe();
}

static uni_error_t on_ready(uni_hid_device_t *device)
{
    int slot = rjm_portal_pairing_slot();
    /* DualShock 4 / DualSense also publish a virtual mouse.  It is not a
       controller slot and must never complete a pending pairing operation. */
    if (uni_hid_device_is_virtual_device(device)) return UNI_ERROR_SUCCESS;
    /* A controller can still page this host after its portal slot was erased:
       the controller remembers our address and BTstack might still have a
       link key.  Do not accept such a stale incoming connection unless the
       user explicitly started pairing. */
    if (slot < 0 && player_for_address(device->conn.btaddr) < 0) {
        drop_bond_for_address(device->conn.btaddr);
        return UNI_ERROR_IGNORE_DEVICE;
    }
    if (slot >= 0) {
        static const uint8_t zero_address[6] = {0};
        if (memcmp(device->conn.btaddr, zero_address, sizeof(zero_address)) == 0)
            return UNI_ERROR_SUCCESS;
        struct RjmPortalSlot old_slots[2];
        struct RjmPortalSlot new_slots[2];
        rjm_portal_get_slots(old_slots);
        rjm_portal_complete_pairing(slot, device->conn.btaddr, device->name);
        rjm_portal_get_slots(new_slots);
        if (old_slots[slot].assigned && memcmp(old_slots[slot].address, device->conn.btaddr, 6) != 0) {
            bool still_registered = false;
            for (int i = 0; i < 2; ++i)
                if (new_slots[i].assigned && memcmp(new_slots[i].address, old_slots[slot].address, 6) == 0)
                    still_registered = true;
            if (!still_registered) drop_bond_for_address(old_slots[slot].address);
        }
    }
    if (slot >= 0)
        uni_bt_bredr_set_defer_security_request(false);
    int player = player_for_address(device->conn.btaddr);
    if (player >= 0 && device->report_parser.set_player_leds)
        device->report_parser.set_player_leds(device, (uint8_t)(1u << player));
    update_ds3_dualsense_throttle(device, NULL);
    update_scan_state_unsafe();
    return UNI_ERROR_SUCCESS;
}

static void on_data(uni_hid_device_t *device, uni_controller_t *controller)
{
    int player = player_for_address(device->conn.btaddr);
    if (player < 0 || controller->klass != UNI_CONTROLLER_CLASS_GAMEPAD) return;
    /* Bluepad32 marks DEVICE_READY only after the platform ready callback.
       Re-evaluate on actual traffic as well, when the complete two-device
       topology is guaranteed to be visible. Once requested successfully the
       tracked handle makes this a no-op on subsequent reports. */
    update_ds3_dualsense_throttle(NULL, NULL);
    ++g_input_reports[player];
    const uni_gamepad_t *gp = &controller->gamepad;
    struct RjmNormalizedState *state = &g_input_state[player];
    uint32_t inputs = 0;
#define INPUT_IF(condition, input) do { if (condition) inputs |= 1u << (input); } while (0)
    INPUT_IF(gp->buttons & BUTTON_A, RJM_INPUT_A);
    INPUT_IF(gp->buttons & BUTTON_B, RJM_INPUT_B);
    INPUT_IF(gp->buttons & BUTTON_X, RJM_INPUT_X);
    INPUT_IF(gp->buttons & BUTTON_Y, RJM_INPUT_Y);
    INPUT_IF(gp->dpad & DPAD_UP, RJM_INPUT_DPAD_UP);
    INPUT_IF(gp->dpad & DPAD_RIGHT, RJM_INPUT_DPAD_RIGHT);
    INPUT_IF(gp->dpad & DPAD_DOWN, RJM_INPUT_DPAD_DOWN);
    INPUT_IF(gp->dpad & DPAD_LEFT, RJM_INPUT_DPAD_LEFT);
    INPUT_IF(gp->buttons & BUTTON_SHOULDER_L, RJM_INPUT_L1);
    INPUT_IF(gp->buttons & BUTTON_SHOULDER_R, RJM_INPUT_R1);
    INPUT_IF((gp->buttons & BUTTON_TRIGGER_L) || gp->brake > 128, RJM_INPUT_L2);
    INPUT_IF((gp->buttons & BUTTON_TRIGGER_R) || gp->throttle > 128, RJM_INPUT_R2);
    INPUT_IF(gp->buttons & BUTTON_THUMB_L, RJM_INPUT_L3);
    INPUT_IF(gp->buttons & BUTTON_THUMB_R, RJM_INPUT_R3);
    INPUT_IF(gp->misc_buttons & MISC_BUTTON_SELECT, RJM_INPUT_SELECT);
    INPUT_IF(gp->misc_buttons & MISC_BUTTON_START, RJM_INPUT_START);
    INPUT_IF(gp->misc_buttons & MISC_BUTTON_SYSTEM, RJM_INPUT_SYSTEM);
    INPUT_IF(gp->misc_buttons & MISC_BUTTON_CAPTURE, RJM_INPUT_MISC);
#undef INPUT_IF
    state->inputs = inputs;
    state->axis[RJM_AXIS_LEFT_X] = normalize_axis(gp->axis_x);
    state->axis[RJM_AXIS_LEFT_Y] = normalize_axis(gp->axis_y);
    state->axis[RJM_AXIS_RIGHT_X] = normalize_axis(gp->axis_rx);
    state->axis[RJM_AXIS_RIGHT_Y] = normalize_axis(gp->axis_ry);
    bool first_report = !g_input_connected[player];
    g_input_connected[player] = true;
    if (first_report) {
        rjm_portal_set_connected(device->conn.btaddr, true);
        update_scan_state_unsafe();
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

static const uni_property_t *get_property(uni_property_idx_t index)
{
    (void)index;
    return NULL;
}

static void on_oob(uni_platform_oob_event_t event, void *data) { (void)event; (void)data; }

struct uni_platform *rjm_bluepad_platform(void)
{
    static struct uni_platform platform = {
        .name = "RemoteJoyMinus Pico 2 W",
        .init = platform_init,
        .on_init_complete = on_init_complete,
        .on_device_discovered = on_discovered,
        .on_device_connected = on_connected,
        .on_device_disconnected = on_disconnected,
        .on_device_ready = on_ready,
        .on_controller_data = on_data,
        .get_property = get_property,
        .on_oob_event = on_oob,
    };
    return &platform;
}

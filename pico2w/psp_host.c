#include "psp_host.h"

#include <string.h>
#include "btstack_run_loop.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "remotejoy_minus_protocol.h"
#include "bluepad_platform.h"

#define CONFIG_TOTAL_MAX 256
#define EVENT_QUEUE_SIZE 32
#define HOSTFS_CMD_SIZE 12
#define HOSTFS_TIMEOUT_MS 2000
#define REPORT_MS 10
#define ANALOG_DEADBAND 2

struct Endpoints {
    uint8_t bulk_in, bulk_cmd, bulk_async;
};
struct Event { int32_t type; uint32_t value; };
struct PlayerState { uint32_t buttons; uint8_t x, y; bool connected; };
enum TransferKind { TRANSFER_NONE, TRANSFER_EVENT, TRANSFER_ANALOG };

static uint8_t g_device_desc[18];
static uint8_t g_config_desc[CONFIG_TOTAL_MAX];
static uint8_t g_xfer_buf[64] TU_ATTR_ALIGNED(4);
static uint8_t g_cmd_buf[64] TU_ATTR_ALIGNED(4);
static tusb_desc_endpoint_t g_in_desc, g_cmd_desc, g_async_desc;
static struct Endpoints g_eps;
static struct Event g_queue[EVENT_QUEUE_SIZE];
static struct Event g_inflight_event;
static struct PlayerState g_state[2];
static struct PlayerState g_last[2];
static uint8_t g_pending_axis[2][2];
static bool g_axis_pending[2][2];
static btstack_timer_source_t g_usb_timer;
static uint8_t g_daddr, g_head, g_tail;
static enum TransferKind g_transfer_kind;
static uint8_t g_inflight_player, g_inflight_axis;
static bool g_handshake_started, g_handshake_complete, g_xfer_busy;
static bool g_input_enabled = true;
static bool g_sending_magic, g_waiting_hello;
static uint32_t g_handshake_ms, g_poll_ms;
static const char *g_status = "未接続";

static bool queue_event(int32_t type, uint32_t value);
static bool queue_button_changes(int player, uint32_t current, uint32_t previous);
static void process_queue(void);
static void context_read(tuh_xfer_t *xfer);

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void reset_host(void)
{
    memset(&g_eps, 0, sizeof(g_eps));
    memset(&g_in_desc, 0, sizeof(g_in_desc));
    memset(&g_cmd_desc, 0, sizeof(g_cmd_desc));
    memset(&g_async_desc, 0, sizeof(g_async_desc));
    g_handshake_started = g_handshake_complete = g_xfer_busy = false;
    g_sending_magic = g_waiting_hello = false;
    g_head = g_tail = 0;
    g_transfer_kind = TRANSFER_NONE;
    memset(g_axis_pending, 0, sizeof(g_axis_pending));
    g_status = "未接続";
    for (int i = 0; i < 2; ++i) {
        g_last[i].buttons = 0; g_last[i].x = g_last[i].y = 128; g_last[i].connected = false;
    }
}

void rjm_psp_host_set_state(int player, const struct RjmMappedState *state, bool connected)
{
    if (player < 0 || player > 1) return;
    if (!g_input_enabled) { state = NULL; connected = false; }
    g_state[player].buttons = connected && state ? state->buttons : 0;
    g_state[player].x = connected && state ? state->x : 128;
    g_state[player].y = connected && state ? state->y : 128;
    g_state[player].connected = connected;
    if (g_handshake_complete) {
        struct PlayerState *last = &g_last[player];
        uint32_t changed = g_state[player].buttons ^ last->buttons;
        if (player && g_state[player].connected != last->connected) {
            if (queue_event(RJM_TYPE_P2_STATUS, g_state[player].connected ? 1 : 0))
                last->connected = g_state[player].connected;
        } else if (!player) last->connected = g_state[player].connected;
        if (!changed || queue_button_changes(player, g_state[player].buttons, last->buttons))
            last->buttons = g_state[player].buttons;
        process_queue();
    }
}

void rjm_psp_host_set_enabled(bool enabled)
{
    g_input_enabled = enabled;
    if (!enabled) {
        for (int i = 0; i < 2; ++i) {
            g_state[i].buttons = 0;
            g_state[i].x = g_state[i].y = 128;
            g_state[i].connected = false;
        }
    }
}

static bool queue_event(int32_t type, uint32_t value)
{
    if ((type == RJM_TYPE_BUTTON_DOWN || type == RJM_TYPE_BUTTON_UP ||
         type == RJM_TYPE_P2_BUTTON_DOWN || type == RJM_TYPE_P2_BUTTON_UP) && value == 0) return true;
    uint8_t next = (uint8_t)((g_tail + 1) % EVENT_QUEUE_SIZE);
    if (next == g_head) return false;
    g_queue[g_tail] = (struct Event){type, value};
    g_tail = next;
    return true;
}

static uint8_t queue_free_count(void)
{
    if (g_tail >= g_head) return (uint8_t)(EVENT_QUEUE_SIZE - 1 - (g_tail - g_head));
    return (uint8_t)(g_head - g_tail - 1);
}

static bool queue_button_changes(int player, uint32_t current, uint32_t previous)
{
    uint32_t changed = current ^ previous;
    uint32_t pressed = changed & current;
    uint32_t released = changed & previous;
    uint8_t required = (pressed != 0) + (released != 0);
    int down = player ? RJM_TYPE_P2_BUTTON_DOWN : RJM_TYPE_BUTTON_DOWN;
    int up = player ? RJM_TYPE_P2_BUTTON_UP : RJM_TYPE_BUTTON_UP;
    if (queue_free_count() < required) return false;
    if (pressed) queue_event(down, pressed);
    if (released) queue_event(up, released);
    return true;
}

static bool submit(uint8_t ep, uint8_t *buf, uint16_t len, tuh_xfer_cb_t cb)
{
    tuh_xfer_t xfer = {.daddr = g_daddr, .ep_addr = ep, .buffer = buf, .buflen = len, .complete_cb = cb};
    return tuh_edpt_xfer(&xfer);
}

static void event_sent(tuh_xfer_t *xfer)
{
    g_xfer_busy = false;
    if (xfer->result == XFER_RESULT_SUCCESS) {
        if (g_transfer_kind == TRANSFER_EVENT) {
            g_head = (uint8_t)((g_head + 1) % EVENT_QUEUE_SIZE);
        } else if (g_transfer_kind == TRANSFER_ANALOG &&
                   g_axis_pending[g_inflight_player][g_inflight_axis] &&
                   g_pending_axis[g_inflight_player][g_inflight_axis] == g_inflight_event.value) {
            g_axis_pending[g_inflight_player][g_inflight_axis] = false;
        }
    }
    g_transfer_kind = TRANSFER_NONE;
    process_queue();
}

static void process_queue(void)
{
    if (!g_handshake_complete || g_xfer_busy) return;
    if (g_head != g_tail) {
        g_inflight_event = g_queue[g_head];
        g_transfer_kind = TRANSFER_EVENT;
    } else {
        g_transfer_kind = TRANSFER_NONE;
        for (int p = 0; p < 2 && g_transfer_kind == TRANSFER_NONE; ++p) {
            for (int axis = 0; axis < 2; ++axis) {
                if (!g_axis_pending[p][axis]) continue;
                g_inflight_player = (uint8_t)p;
                g_inflight_axis = (uint8_t)axis;
                g_inflight_event.type = p ? (axis ? RJM_TYPE_P2_ANALOG_Y : RJM_TYPE_P2_ANALOG_X)
                                                : (axis ? RJM_TYPE_ANALOG_Y : RJM_TYPE_ANALOG_X);
                g_inflight_event.value = g_pending_axis[p][axis];
                g_transfer_kind = TRANSFER_ANALOG;
                break;
            }
        }
        if (g_transfer_kind == TRANSFER_NONE) return;
    }
    size_t length = rjm_build_async_joy_event(g_xfer_buf, g_inflight_event.type, g_inflight_event.value);
    if (submit(g_eps.bulk_async, g_xfer_buf, (uint16_t)length, event_sent)) {
        g_xfer_busy = true;
    } else {
        g_transfer_kind = TRANSFER_NONE;
    }
}

static void hello_response_sent(tuh_xfer_t *xfer)
{
    if (xfer->result != XFER_RESULT_SUCCESS) { g_status = "応答送信エラー"; return; }
    g_handshake_complete = true;
    g_status = "接続済み";
    g_head = g_tail = 0;
    for (int i = 0; i < 2; ++i) {
        g_last[i].buttons = 0; g_last[i].x = g_last[i].y = 128; g_last[i].connected = false;
    }
    memset(g_cmd_buf, 0, sizeof(g_cmd_buf));
    submit(g_eps.bulk_in, g_cmd_buf, HOSTFS_CMD_SIZE, context_read);
}

static void context_response_sent(tuh_xfer_t *xfer)
{
    (void)xfer;
}

static void context_read(tuh_xfer_t *xfer)
{
    if (xfer->result != XFER_RESULT_SUCCESS || xfer->actual_len < HOSTFS_CMD_SIZE ||
        le32(g_cmd_buf) != RJM_HOSTFS_MAGIC || le32(g_cmd_buf + 4) != RJM_HOSTFS_CMD_CONTEXT)
        return;
    uint32_t context = le32(g_cmd_buf + 8);
    rjm_bluepad_set_pops_context((context & RJM_CONTEXT_POPS) != 0);
    size_t length = rjm_build_hostfs_response(g_cmd_buf, RJM_HOSTFS_CMD_CONTEXT, context);
    submit(g_eps.bulk_cmd, g_cmd_buf, (uint16_t)length, context_response_sent);
}

static void hello_read(tuh_xfer_t *xfer)
{
    g_waiting_hello = false;
    if (xfer->result != XFER_RESULT_SUCCESS || xfer->actual_len < HOSTFS_CMD_SIZE ||
        le32(g_xfer_buf) != RJM_HOSTFS_MAGIC || le32(g_xfer_buf + 4) != RJM_HOSTFS_CMD_HELLO ||
        le32(g_xfer_buf + 8) != 0) { g_status = "HostFS応答エラー"; return; }
    size_t length = rjm_build_hostfs_response(g_xfer_buf, RJM_HOSTFS_CMD_HELLO,
                                               RJM_HOSTFS_CAP_CONTEXT);
    submit(g_eps.bulk_cmd, g_xfer_buf, (uint16_t)length, hello_response_sent);
}

static void magic_sent(tuh_xfer_t *xfer)
{
    g_sending_magic = false;
    if (xfer->result != XFER_RESULT_SUCCESS) return;
    memset(g_xfer_buf, 0, sizeof(g_xfer_buf));
    g_waiting_hello = true;
    g_handshake_ms = to_ms_since_boot(get_absolute_time());
    if (!submit(g_eps.bulk_in, g_xfer_buf, HOSTFS_CMD_SIZE, hello_read)) g_waiting_hello = false;
}

static void send_magic(void)
{
    g_sending_magic = true;
    g_waiting_hello = false;
    g_handshake_ms = to_ms_since_boot(get_absolute_time());
    size_t length = rjm_build_hostfs_magic(g_xfer_buf);
    if (!submit(g_eps.bulk_cmd, g_xfer_buf, (uint16_t)length, magic_sent)) g_sending_magic = false;
}

static void start_handshake(uint8_t daddr)
{
    if (g_handshake_started) return;
    if (!g_eps.bulk_in || !g_eps.bulk_cmd || !g_eps.bulk_async) {
        g_status = "RemoteJoyインターフェースなし";
        return;
    }
    g_handshake_started = true; g_daddr = daddr;
    g_status = "ハンドシェイク中";
    if (!tuh_edpt_open(daddr, &g_in_desc) || !tuh_edpt_open(daddr, &g_cmd_desc) ||
        !tuh_edpt_open(daddr, &g_async_desc)) { g_handshake_started = false; return; }
    send_magic();
}

static void parse_config(uint8_t daddr, uint16_t length)
{
    bool target = false;
    for (uint16_t offset = 0; offset + 2 <= length;) {
        const uint8_t *p = g_config_desc + offset;
        uint8_t len = p[0];
        if (len < 2 || offset + len > length) break;
        if (p[1] == TUSB_DESC_INTERFACE && len >= 9)
            target = p[5] == 0xff && p[6] == 0x01 && p[7] == 0xff;
        else if (target && p[1] == TUSB_DESC_ENDPOINT && len >= 7 && (p[3] & 3) == TUSB_XFER_BULK) {
            tusb_desc_endpoint_t *dest;
            if (p[2] & 0x80) { g_eps.bulk_in = p[2]; dest = &g_in_desc; }
            else if (!g_eps.bulk_cmd) { g_eps.bulk_cmd = p[2]; dest = &g_cmd_desc; }
            else { g_eps.bulk_async = p[2]; dest = &g_async_desc; }
            memset(dest, 0, sizeof(*dest)); memcpy(dest, p, len < sizeof(*dest) ? len : sizeof(*dest));
        }
        offset += len;
    }
    start_handshake(daddr);
}

static void config_read(tuh_xfer_t *xfer)
{
    if (xfer->result != XFER_RESULT_SUCCESS) { g_status = "構成取得エラー"; return; }
    uint16_t length = le16(g_config_desc + 2);
    if (length > xfer->actual_len) length = (uint16_t)xfer->actual_len;
    parse_config(xfer->daddr, length);
}

static void device_read(tuh_xfer_t *xfer)
{
    if (xfer->result == XFER_RESULT_SUCCESS)
        tuh_descriptor_get_configuration(xfer->daddr, 0, g_config_desc, sizeof(g_config_desc), config_read, 0);
    else g_status = "デバイス取得エラー";
}

void tuh_mount_cb(uint8_t daddr)
{
    reset_host(); g_daddr = daddr;
    g_status = "列挙中";
    tuh_descriptor_get_device(daddr, g_device_desc, sizeof(g_device_desc), device_read, 0);
}
void tuh_umount_cb(uint8_t daddr) { (void)daddr; reset_host(); }

const char *rjm_psp_host_status(void) { return g_status; }
bool rjm_psp_host_is_connected(void) { return g_handshake_complete; }

static int abs_int(int value) { return value < 0 ? -value : value; }
static void poll_state(void)
{
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!g_handshake_complete || now - g_poll_ms < REPORT_MS) return;
    g_poll_ms = now;
    for (int p = 0; p < 2; ++p) {
        struct PlayerState state = g_state[p];
        struct PlayerState *last = &g_last[p];
        uint32_t changed = state.buttons ^ last->buttons;
        if (p && state.connected != last->connected) {
            if (queue_event(RJM_TYPE_P2_STATUS, state.connected ? 1 : 0)) last->connected = state.connected;
        } else if (!p) last->connected = state.connected;
        if (!changed || queue_button_changes(p, state.buttons, last->buttons)) last->buttons = state.buttons;
        if (abs_int((int)state.x - last->x) >= ANALOG_DEADBAND) {
            g_pending_axis[p][0] = state.x; g_axis_pending[p][0] = true; last->x = state.x;
        }
        if (abs_int((int)state.y - last->y) >= ANALOG_DEADBAND) {
            g_pending_axis[p][1] = state.y; g_axis_pending[p][1] = true; last->y = state.y;
        }
    }
}

static void usb_timer(btstack_timer_source_t *timer)
{
    tuh_task();
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (g_handshake_started && !g_handshake_complete && now - g_handshake_ms >= HOSTFS_TIMEOUT_MS) {
        if (g_waiting_hello) tuh_edpt_abort_xfer(g_daddr, g_eps.bulk_in);
        else if (g_sending_magic) tuh_edpt_abort_xfer(g_daddr, g_eps.bulk_cmd);
        g_sending_magic = g_waiting_hello = false;
        send_magic();
    }
    poll_state(); process_queue();
    btstack_run_loop_set_timer(timer, 1);
    btstack_run_loop_add_timer(timer);
}

void rjm_psp_host_init(void)
{
    g_input_enabled = true;
    for (int i = 0; i < 2; ++i) g_state[i] = (struct PlayerState){0, 128, 128, false};
    tusb_init(); reset_host();
    btstack_run_loop_set_timer_handler(&g_usb_timer, usb_timer);
    btstack_run_loop_set_timer(&g_usb_timer, 1);
    btstack_run_loop_add_timer(&g_usb_timer);
}

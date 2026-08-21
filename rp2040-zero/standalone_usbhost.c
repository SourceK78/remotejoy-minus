/*
 * standalone_usbhost.c - RP2040 TinyUSB host sender for RemoteJoyMinus
 */
#include <stdio.h>
#include <string.h>
#include "bsp/board.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "remotejoy_minus_protocol.h"

#define CONFIG_TOTAL_MAX 256
#define HOSTFS_CMD_SIZE 12
#define HOSTFS_MAGIC_SIZE 4
#define HOSTFS_HELLO_TIMEOUT_MS 2000
#define HOSTFS_MAGIC_TIMEOUT_MS 2000

#define STATE_REPORT_MS 10
#define STATE_HEARTBEAT_MS 0
#define BUTTON_REASSERT_MS 80
#define EVENT_QUEUE_SIZE 16
#define ENABLE_INPUT_SCAN 1
#define ENABLE_JOY_EVENT_TRACE 0

#define INPUT_SOURCE_GPIO 0
#define INPUT_SOURCE_PS2  1
#define INPUT_SOURCE      INPUT_SOURCE_PS2

#define ANALOG_X_PIN 26
#define ANALOG_Y_PIN 27
#define ANALOG_X_INPUT 0
#define ANALOG_Y_INPUT 1
#define ANALOG_ADC_MIN 0
#define ANALOG_ADC_CENTER 2048
#define ANALOG_ADC_MAX 4095
#define ANALOG_X_INVERT 0
#define ANALOG_Y_INVERT 0
#define ANALOG_DEADBAND 2

#define BUTTON_COUNT 12

#define PS2_PIN_DAT 5
#define PS2_PIN_CMD 6
#define PS2_PIN_ATT 7
#define PS2_PIN_CLK 8
#define PS2_PIN_ACK 26
#define PS2_USE_ACK 0
#define PS2_CLOCK_DELAY_US 4
#define PS2_COMMAND_DELAY_US 20
#define PS2_ANALOG_RETRY_WINDOW_MS 5000
#define PS2_ANALOG_RETRY_INTERVAL_MS 500
#define PS2_RESPONSE_SIZE 9
#define PS2_MULTITAP_SLOT_COUNT 2
#define PS2_MULTITAP_LONG_RESPONSE_SIZE 35
#define PS2_MULTITAP_LONG_SLOT_SIZE 8
#define PS2_MULTITAP_LONG_PROBE_INTERVAL_MS 500
#define PS2_RESP_MODE 1
#define PS2_RESP_ID 2
#define PS2_RESP_BUTTON_LO 3
#define PS2_RESP_BUTTON_HI 4
#define PS2_RESP_RIGHT_X 5
#define PS2_RESP_RIGHT_Y 6
#define PS2_RESP_LEFT_X 7
#define PS2_RESP_LEFT_Y 8
#define PS2_ANALOG_CENTER 128
#define PS2_STICK_LOW 80
#define PS2_STICK_HIGH 176
#define PS2_FACE_SWAP_STICK_LOW 104
#define PS2_FACE_SWAP_STICK_HIGH 152

#define STATUS_LED_PIN_R 27
#define STATUS_LED_PIN_G 28
#define STATUS_LED_PIN_B 29
#define STATUS_LED_ACTIVE_LOW 0

#define PS2_BTN_SELECT   0x0001UL
#define PS2_BTN_L3       0x0002UL
#define PS2_BTN_R3       0x0004UL
#define PS2_BTN_START    0x0008UL
#define PS2_BTN_UP       0x0010UL
#define PS2_BTN_RIGHT    0x0020UL
#define PS2_BTN_DOWN     0x0040UL
#define PS2_BTN_LEFT     0x0080UL
#define PS2_BTN_L2       0x0100UL
#define PS2_BTN_R2       0x0200UL
#define PS2_BTN_L1       0x0400UL
#define PS2_BTN_R1       0x0800UL
#define PS2_BTN_TRIANGLE 0x1000UL
#define PS2_BTN_CIRCLE   0x2000UL
#define PS2_BTN_CROSS    0x4000UL
#define PS2_BTN_SQUARE   0x8000UL

struct UsbHostFsEndpoints
{
	uint8_t interface_number;
	uint8_t ep_bulk_in;
	uint8_t ep_bulk_out_cmd;
	uint8_t ep_bulk_out_async;
	uint16_t bulk_in_mps;
	uint16_t bulk_out_cmd_mps;
	uint16_t bulk_out_async_mps;
};

struct ButtonMap
{
	uint8_t gpio;
	uint32_t mask;
};

struct JoyEventQueueItem
{
	int32_t type;
	uint32_t value;
};

struct ControllerState
{
	uint32_t buttons;
	uint8_t x;
	uint8_t y;
};

static const struct ButtonMap g_buttons[BUTTON_COUNT] = {
	{ 2, PSP_CTRL_CROSS },
	{ 3, PSP_CTRL_CIRCLE },
	{ 4, PSP_CTRL_SQUARE },
	{ 5, PSP_CTRL_TRIANGLE },
	{ 6, PSP_CTRL_UP },
	{ 7, PSP_CTRL_DOWN },
	{ 8, PSP_CTRL_LEFT },
	{ 9, PSP_CTRL_RIGHT },
	{ 10, PSP_CTRL_START },
	{ 11, PSP_CTRL_SELECT },
	{ 12, PSP_CTRL_LTRIGGER },
	{ 13, PSP_CTRL_RTRIGGER },
};

static uint8_t g_desc_device[18];
static uint8_t g_desc_config[CONFIG_TOTAL_MAX];
static uint8_t g_xfer_buf[64] TU_ATTR_ALIGNED(4);
static struct UsbHostFsEndpoints g_eps;
static tusb_desc_endpoint_t g_ep_bulk_in_desc;
static tusb_desc_endpoint_t g_ep_bulk_out_cmd_desc;
static tusb_desc_endpoint_t g_ep_bulk_out_async_desc;
static uint8_t g_daddr;
static int g_handshake_started;
static int g_handshake_complete;
static int g_sending_magic;
static int g_waiting_for_hello;
static uint32_t g_handshake_last_ms;
static int g_input_started;
static int g_async_xfer_busy;
static uint32_t g_last_buttons;
static uint8_t g_last_x = 128;
static uint8_t g_last_y = 128;
static int g_p2_present;
static int g_last_p2_present = -1;
static struct ControllerState g_p2_state;
static uint32_t g_last_p2_buttons;
static uint8_t g_last_p2_x = 128;
static uint8_t g_last_p2_y = 128;
static uint32_t g_last_poll_ms;
static uint32_t g_last_heartbeat_ms;
static uint32_t g_last_button_reassert_ms;
static int g_status_led_initialized;
static int g_status_led_last_color = -1;
static int g_popn_music_controller_mode;
enum RightStickMode
{
	RIGHT_STICK_MODE_OFF = 0,
	RIGHT_STICK_MODE_DPAD,
	RIGHT_STICK_MODE_FACE_SWAP,
	RIGHT_STICK_MODE_STAR_SOLDIER,
	RIGHT_STICK_MODE_COUNT
};

static int g_right_stick_mode = RIGHT_STICK_MODE_OFF;
static int g_last_ps2_r3_pressed;
static uint32_t g_ps2_analog_retry_start_ms;
static uint32_t g_ps2_analog_retry_last_ms;
static int g_ps2_slot_present[PS2_MULTITAP_SLOT_COUNT];
static uint32_t g_ps2_slot_last_buttons[PS2_MULTITAP_SLOT_COUNT];
static uint8_t g_ps2_slot_last_x[PS2_MULTITAP_SLOT_COUNT];
static uint8_t g_ps2_slot_last_y[PS2_MULTITAP_SLOT_COUNT];
static int g_ps2_multitap_long_active;
static uint32_t g_ps2_multitap_long_probe_last_ms;
static struct JoyEventQueueItem g_event_queue[EVENT_QUEUE_SIZE];
static uint8_t g_event_head;
static uint8_t g_event_tail;

static void hostfs_magic_sent_cb(tuh_xfer_t *xfer);
static void hostfs_hello_read_cb(tuh_xfer_t *xfer);
static void hostfs_hello_response_sent_cb(tuh_xfer_t *xfer);
static void joy_event_sent_cb(tuh_xfer_t *xfer);
static void clear_controller_state_value(struct ControllerState *state);

enum StatusLedColor
{
	STATUS_LED_RED = 0,
	STATUS_LED_GREEN,
	STATUS_LED_BLUE,
	STATUS_LED_MAGENTA,
	STATUS_LED_CYAN,
	STATUS_LED_WHITE
};

static void status_led_put_pin(uint8_t pin, int on)
{
	gpio_put(pin, STATUS_LED_ACTIVE_LOW ? !on : on);
}

static void status_led_set_color(int color)
{
	int red = 0;
	int green = 0;
	int blue = 0;

	if(!g_status_led_initialized || color == g_status_led_last_color)
	{
		return;
	}

	switch(color)
	{
		case STATUS_LED_GREEN:
			green = 1;
			break;
		case STATUS_LED_BLUE:
			blue = 1;
			break;
		case STATUS_LED_MAGENTA:
			red = 1;
			blue = 1;
			break;
		case STATUS_LED_CYAN:
			green = 1;
			blue = 1;
			break;
		case STATUS_LED_WHITE:
			red = 1;
			green = 1;
			blue = 1;
			break;
		case STATUS_LED_RED:
		default:
			red = 1;
			break;
	}

	status_led_put_pin(STATUS_LED_PIN_R, red);
	status_led_put_pin(STATUS_LED_PIN_G, green);
	status_led_put_pin(STATUS_LED_PIN_B, blue);
	g_status_led_last_color = color;
}

static void update_status_led(void)
{
	if(!g_handshake_complete)
	{
		status_led_set_color(STATUS_LED_RED);
		return;
	}

	if(g_popn_music_controller_mode)
	{
		status_led_set_color(STATUS_LED_WHITE);
		return;
	}

	switch(g_right_stick_mode)
	{
		case RIGHT_STICK_MODE_DPAD:
			status_led_set_color(STATUS_LED_GREEN);
			break;
		case RIGHT_STICK_MODE_FACE_SWAP:
			status_led_set_color(STATUS_LED_MAGENTA);
			break;
		case RIGHT_STICK_MODE_STAR_SOLDIER:
			status_led_set_color(STATUS_LED_CYAN);
			break;
		case RIGHT_STICK_MODE_OFF:
		default:
			status_led_set_color(STATUS_LED_BLUE);
			break;
	}
}

static void init_status_led(void)
{
	gpio_init(STATUS_LED_PIN_R);
	gpio_set_dir(STATUS_LED_PIN_R, GPIO_OUT);
	gpio_init(STATUS_LED_PIN_G);
	gpio_set_dir(STATUS_LED_PIN_G, GPIO_OUT);
	gpio_init(STATUS_LED_PIN_B);
	gpio_set_dir(STATUS_LED_PIN_B, GPIO_OUT);

	g_status_led_initialized = 1;
	g_status_led_last_color = -1;
	update_status_led();
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
		((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void clear_endpoints(void)
{
	memset(&g_eps, 0, sizeof(g_eps));
	memset(&g_ep_bulk_in_desc, 0, sizeof(g_ep_bulk_in_desc));
	memset(&g_ep_bulk_out_cmd_desc, 0, sizeof(g_ep_bulk_out_cmd_desc));
	memset(&g_ep_bulk_out_async_desc, 0, sizeof(g_ep_bulk_out_async_desc));
	g_eps.interface_number = 0xFF;
	g_handshake_started = 0;
	g_handshake_complete = 0;
	g_sending_magic = 0;
	g_waiting_for_hello = 0;
	g_handshake_last_ms = 0;
	g_input_started = 0;
	g_async_xfer_busy = 0;
	g_last_buttons = 0;
	g_last_x = 128;
	g_last_y = 128;
	g_p2_present = 0;
	g_last_p2_present = -1;
	clear_controller_state_value(&g_p2_state);
	g_last_p2_buttons = 0;
	g_last_p2_x = 128;
	g_last_p2_y = 128;
	g_last_poll_ms = 0;
	g_last_heartbeat_ms = 0;
	g_last_button_reassert_ms = 0;
	g_last_ps2_r3_pressed = 0;
	g_popn_music_controller_mode = 0;
	memset(g_ps2_slot_present, 0, sizeof(g_ps2_slot_present));
	memset(g_ps2_slot_last_buttons, 0, sizeof(g_ps2_slot_last_buttons));
	memset(g_ps2_slot_last_x, 128, sizeof(g_ps2_slot_last_x));
	memset(g_ps2_slot_last_y, 128, sizeof(g_ps2_slot_last_y));
	g_ps2_multitap_long_active = 0;
	g_ps2_multitap_long_probe_last_ms = 0;
	g_event_head = 0;
	g_event_tail = 0;
	update_status_led();
}

static void reset_input_diff_state(void)
{
	g_last_buttons = 0;
	g_last_x = 128;
	g_last_y = 128;
	g_p2_present = 0;
	g_last_p2_present = -1;
	clear_controller_state_value(&g_p2_state);
	g_last_p2_buttons = 0;
	g_last_p2_x = 128;
	g_last_p2_y = 128;
	g_last_poll_ms = 0;
	g_last_heartbeat_ms = 0;
	g_last_button_reassert_ms = 0;
	g_last_ps2_r3_pressed = 0;
	memset(g_ps2_slot_present, 0, sizeof(g_ps2_slot_present));
	memset(g_ps2_slot_last_buttons, 0, sizeof(g_ps2_slot_last_buttons));
	memset(g_ps2_slot_last_x, 128, sizeof(g_ps2_slot_last_x));
	memset(g_ps2_slot_last_y, 128, sizeof(g_ps2_slot_last_y));
	g_ps2_multitap_long_active = 0;
	g_ps2_multitap_long_probe_last_ms = 0;
}

static int is_bulk_endpoint(const uint8_t *desc)
{
	return desc[1] == TUSB_DESC_ENDPOINT && ((desc[3] & 0x03) == TUSB_XFER_BULK);
}

static void print_endpoint(const uint8_t *desc)
{
	uint8_t addr = desc[2];
	uint16_t mps = le16(&desc[4]);

	printf("    endpoint 0x%02X attr 0x%02X mps %u\n", addr, desc[3], mps);
}

static void save_endpoint_desc(tusb_desc_endpoint_t *out, const uint8_t *desc)
{
	memset(out, 0, sizeof(*out));
	memcpy(out, desc, desc[0] < sizeof(*out) ? desc[0] : sizeof(*out));
}

static int submit_xfer(uint8_t ep_addr, uint8_t *buffer, uint16_t len, tuh_xfer_cb_t cb)
{
	tuh_xfer_t xfer;

	memset(&xfer, 0, sizeof(xfer));
	xfer.daddr = g_daddr;
	xfer.ep_addr = ep_addr;
	xfer.buffer = buffer;
	xfer.buflen = len;
	xfer.complete_cb = cb;

	return tuh_edpt_xfer(&xfer);
}

static void send_hostfs_magic(void)
{
	size_t len;

	g_waiting_for_hello = 0;
	g_sending_magic = 1;
	g_handshake_last_ms = to_ms_since_boot(get_absolute_time());
	len = rjm_build_hostfs_magic(g_xfer_buf);
	printf("Sending HOSTFS magic to endpoint 0x%02X\n", g_eps.ep_bulk_out_cmd);
	if(!submit_xfer(g_eps.ep_bulk_out_cmd, g_xfer_buf, (uint16_t) len, hostfs_magic_sent_cb))
	{
		g_sending_magic = 0;
		printf("Failed to submit HOSTFS magic transfer\n");
	}
}

static void start_hostfs_hello(uint8_t daddr)
{
	if(g_handshake_started)
	{
		return;
	}
	g_handshake_started = 1;
	g_daddr = daddr;

	if(!g_eps.ep_bulk_in || !g_eps.ep_bulk_out_cmd || !g_eps.ep_bulk_out_async)
	{
		printf("Cannot start HostFS hello: missing endpoint\n");
		return;
	}

	if(!tuh_edpt_open(daddr, &g_ep_bulk_in_desc))
	{
		printf("Failed to open bulk IN endpoint 0x%02X\n", g_eps.ep_bulk_in);
		return;
	}
	if(!tuh_edpt_open(daddr, &g_ep_bulk_out_cmd_desc))
	{
		printf("Failed to open command OUT endpoint 0x%02X\n", g_eps.ep_bulk_out_cmd);
		return;
	}
	if(!tuh_edpt_open(daddr, &g_ep_bulk_out_async_desc))
	{
		printf("Failed to open async OUT endpoint 0x%02X\n", g_eps.ep_bulk_out_async);
		return;
	}

	send_hostfs_magic();
}

static int clamp_int(int value, int min_value, int max_value)
{
	if(value < min_value)
	{
		return min_value;
	}
	if(value > max_value)
	{
		return max_value;
	}

	return value;
}

static int abs_int(int value)
{
	return value < 0 ? -value : value;
}

static uint8_t scale_axis_to_u8(int value, int invert)
{
	long out;

	value = clamp_int(value, ANALOG_ADC_MIN, ANALOG_ADC_MAX);

	if(value >= ANALOG_ADC_CENTER)
	{
		long span = ANALOG_ADC_MAX - ANALOG_ADC_CENTER;
		out = span <= 0 ? 128 :
			128 + (((long) (value - ANALOG_ADC_CENTER) * 127L + (span / 2)) / span);
	}
	else
	{
		long span = ANALOG_ADC_CENTER - ANALOG_ADC_MIN;
		out = span <= 0 ? 128 :
			128 - (((long) (ANALOG_ADC_CENTER - value) * 128L + (span / 2)) / span);
	}

	out = clamp_int((int) out, 0, 255);
	if(invert)
	{
		out = 255 - out;
	}

	return (uint8_t) out;
}

static uint8_t read_axis(uint8_t input, int invert)
{
	adc_select_input(input);
	return scale_axis_to_u8(adc_read(), invert);
}

static uint32_t read_buttons(void)
{
	uint32_t buttons = 0;
	int i;

	for(i = 0; i < BUTTON_COUNT; i++)
	{
		if(!gpio_get(g_buttons[i].gpio))
		{
			buttons |= g_buttons[i].mask;
		}
	}

	return buttons;
}

static void read_gpio_controller_state(struct ControllerState *state)
{
	state->buttons = read_buttons();
	state->x = read_axis(ANALOG_X_INPUT, ANALOG_X_INVERT);
	state->y = read_axis(ANALOG_Y_INPUT, ANALOG_Y_INVERT);
}

static void init_input_pins(void)
{
	int i;

#if INPUT_SOURCE == INPUT_SOURCE_GPIO
	for(i = 0; i < BUTTON_COUNT; i++)
	{
		gpio_init(g_buttons[i].gpio);
		gpio_set_dir(g_buttons[i].gpio, GPIO_IN);
		gpio_pull_up(g_buttons[i].gpio);
	}

	adc_init();
	adc_gpio_init(ANALOG_X_PIN);
	adc_gpio_init(ANALOG_Y_PIN);
#else
	(void) i;
#endif
}

static uint8_t ps2_transfer_byte(uint8_t out)
{
	uint8_t in = 0;
	int bit;

	for(bit = 0; bit < 8; bit++)
	{
		gpio_put(PS2_PIN_CMD, out & 1);
		busy_wait_us_32(PS2_CLOCK_DELAY_US);

		gpio_put(PS2_PIN_CLK, 0);
		busy_wait_us_32(PS2_CLOCK_DELAY_US);

		if(gpio_get(PS2_PIN_DAT))
		{
			in |= (uint8_t) (1u << bit);
		}

		gpio_put(PS2_PIN_CLK, 1);
		busy_wait_us_32(PS2_CLOCK_DELAY_US);
		out >>= 1;
	}

	gpio_put(PS2_PIN_CMD, 1);
	busy_wait_us_32(PS2_COMMAND_DELAY_US);
	return in;
}

static void ps2_transaction(const uint8_t *tx, uint8_t *rx, size_t len)
{
	size_t i;

	gpio_put(PS2_PIN_CLK, 1);
	gpio_put(PS2_PIN_CMD, 1);
	gpio_put(PS2_PIN_ATT, 0);
	busy_wait_us_32(PS2_COMMAND_DELAY_US);

	for(i = 0; i < len; i++)
	{
		rx[i] = ps2_transfer_byte(tx[i]);
	}

	gpio_put(PS2_PIN_ATT, 1);
	gpio_put(PS2_PIN_CMD, 1);
	gpio_put(PS2_PIN_CLK, 1);
	busy_wait_us_32(PS2_COMMAND_DELAY_US);
}

static int ps2_poll_raw_addr(uint8_t addr, uint8_t *rx)
{
	uint8_t poll_cmd[PS2_RESPONSE_SIZE] = {
		addr, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	memset(rx, 0, PS2_RESPONSE_SIZE);
	ps2_transaction(poll_cmd, rx, PS2_RESPONSE_SIZE);

	if(rx[PS2_RESP_MODE] == 0x00 || rx[PS2_RESP_MODE] == 0xFF ||
		rx[PS2_RESP_ID] != 0x5A)
	{
		return 0;
	}

	return 1;
}

static int ps2_poll_raw(uint8_t *rx)
{
	return ps2_poll_raw_addr(0x01, rx);
}

static void ps2_send_config_command_addr(uint8_t addr, const uint8_t *cmd, size_t len)
{
	uint8_t rx[PS2_RESPONSE_SIZE];
	uint8_t tx[PS2_RESPONSE_SIZE];

	memset(rx, 0, sizeof(rx));
	memset(tx, 0, sizeof(tx));
	memcpy(tx, cmd, len > sizeof(tx) ? sizeof(tx) : len);
	tx[0] = addr;
	ps2_transaction(tx, rx, len);
}

static void ps2_send_config_command(const uint8_t *cmd, size_t len)
{
	ps2_send_config_command_addr(0x01, cmd, len);
}

static void ps2_enable_analog_mode_addr(uint8_t addr)
{
	static const uint8_t enter_config[5] = {
		0x01, 0x43, 0x00, 0x01, 0x00
	};
	static const uint8_t set_analog[9] = {
		0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00
	};
	static const uint8_t exit_config[9] = {
		0x01, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A
	};

	ps2_send_config_command_addr(addr, enter_config, sizeof(enter_config));
	ps2_send_config_command_addr(addr, set_analog, sizeof(set_analog));
	ps2_send_config_command_addr(addr, exit_config, sizeof(exit_config));
}

static void ps2_enable_analog_mode(void)
{
	ps2_enable_analog_mode_addr(0x01);
}

static int ps2_is_analog_mode(uint8_t mode)
{
	return mode == 0x53 || mode == 0x73 || mode == 0x79;
}

static void ps2_retry_analog_mode(void)
{
	uint32_t now = to_ms_since_boot(get_absolute_time());

	if(g_ps2_analog_retry_start_ms == 0)
	{
		return;
	}
	if((uint32_t) (now - g_ps2_analog_retry_start_ms) > PS2_ANALOG_RETRY_WINDOW_MS)
	{
		g_ps2_analog_retry_start_ms = 0;
		return;
	}
	if((uint32_t) (now - g_ps2_analog_retry_last_ms) < PS2_ANALOG_RETRY_INTERVAL_MS)
	{
		return;
	}

	g_ps2_analog_retry_last_ms = now;
	ps2_enable_analog_mode();
	ps2_enable_analog_mode_addr(0x02);
}

static void ps2_start_analog_retry(void)
{
	uint32_t now = to_ms_since_boot(get_absolute_time());

	g_ps2_analog_retry_start_ms = now;
	g_ps2_analog_retry_last_ms = now - PS2_ANALOG_RETRY_INTERVAL_MS;
}

static int ps2_is_popn_music_controller(uint32_t ps2)
{
	return (ps2 & (PS2_BTN_LEFT | PS2_BTN_RIGHT | PS2_BTN_DOWN)) ==
		(PS2_BTN_LEFT | PS2_BTN_RIGHT | PS2_BTN_DOWN);
}

static uint32_t ps2_popn_music_buttons_to_psp(uint32_t ps2)
{
	uint32_t psp = 0;
	int select_l1_combo = (ps2 & PS2_BTN_SELECT) && (ps2 & PS2_BTN_L1);

	/* Matches psp-1000-control.ino remapPopnMusicButtons(). */
	if(ps2 & PS2_BTN_UP)       psp |= PSP_CTRL_RIGHT;
	if(ps2 & PS2_BTN_CIRCLE)   psp |= PSP_CTRL_LEFT;
	if(ps2 & PS2_BTN_CROSS)    psp |= PSP_CTRL_UP;
	if(ps2 & PS2_BTN_SQUARE)   psp |= PSP_CTRL_DOWN;
	if(ps2 & PS2_BTN_TRIANGLE) psp |= PSP_CTRL_TRIANGLE;
	if(ps2 & PS2_BTN_R1)       psp |= PSP_CTRL_LTRIGGER;
	if(select_l1_combo)        psp |= PSP_CTRL_SQUARE;
	if(!select_l1_combo && (ps2 & PS2_BTN_L1)) psp |= PSP_CTRL_CIRCLE;
	if(ps2 & PS2_BTN_R2)       psp |= PSP_CTRL_RTRIGGER;
	if(ps2 & PS2_BTN_L2)       psp |= PSP_CTRL_CROSS;
	if(ps2 & PS2_BTN_START)    psp |= PSP_CTRL_START;
	if(!select_l1_combo && (ps2 & PS2_BTN_SELECT)) psp |= PSP_CTRL_SELECT;

	return psp;
}

static void update_popn_music_controller_mode(uint32_t ps2_buttons)
{
	int detected = ps2_is_popn_music_controller(ps2_buttons);

	if(detected && !g_popn_music_controller_mode)
	{
		g_popn_music_controller_mode = 1;
		g_right_stick_mode = RIGHT_STICK_MODE_OFF;
		printf("Pop'n Music controller mode enabled\n");
		update_status_led();
	}
	else if(!detected && g_popn_music_controller_mode)
	{
		g_popn_music_controller_mode = 0;
		g_right_stick_mode = RIGHT_STICK_MODE_OFF;
		printf("Pop'n Music controller mode disabled\n");
		update_status_led();
	}
}

static uint32_t ps2_buttons_to_psp(uint32_t ps2)
{
	uint32_t psp = 0;

	if(ps2 & PS2_BTN_SELECT)   psp |= PSP_CTRL_SELECT;
	if(ps2 & PS2_BTN_START)    psp |= PSP_CTRL_START;
	if(g_right_stick_mode == RIGHT_STICK_MODE_STAR_SOLDIER)
	{
		if(ps2 & PS2_BTN_UP)    psp |= PSP_CTRL_RIGHT;
		if(ps2 & PS2_BTN_DOWN)  psp |= PSP_CTRL_LEFT;
		if(ps2 & PS2_BTN_LEFT)  psp |= PSP_CTRL_UP;
		if((ps2 & PS2_BTN_RIGHT) && !(ps2 & PS2_BTN_START)) psp |= PSP_CTRL_DOWN;
	}
	else
	{
		if(ps2 & PS2_BTN_UP)       psp |= PSP_CTRL_UP;
		if((ps2 & PS2_BTN_RIGHT) && !(ps2 & PS2_BTN_START)) psp |= PSP_CTRL_RIGHT;
		if(ps2 & PS2_BTN_DOWN)     psp |= PSP_CTRL_DOWN;
		if(ps2 & PS2_BTN_LEFT)     psp |= PSP_CTRL_LEFT;
	}
	if(ps2 & PS2_BTN_L1)       psp |= PSP_CTRL_LTRIGGER;
	if(ps2 & PS2_BTN_R1)       psp |= PSP_CTRL_RTRIGGER;
	if(ps2 & PS2_BTN_L2)       psp |= PSP_CTRL_VOLDOWN;
	if(ps2 & PS2_BTN_R2)       psp |= PSP_CTRL_VOLUP;
	if(ps2 & PS2_BTN_L3)       psp |= PSP_CTRL_NOTE;
	if((ps2 & PS2_BTN_START) && (ps2 & PS2_BTN_RIGHT)) psp |= PSP_CTRL_HOME;
	/* R3 is HOME unless START is held; START+R3 toggles the controller mapping mode. */
	if((ps2 & PS2_BTN_R3) && !(ps2 & PS2_BTN_START)) psp |= PSP_CTRL_HOME;
	if(g_right_stick_mode == RIGHT_STICK_MODE_FACE_SWAP)
	{
		if(ps2 & PS2_BTN_TRIANGLE) psp |= PSP_CTRL_UP;
		if(ps2 & PS2_BTN_CIRCLE)   psp |= PSP_CTRL_RIGHT;
		if(ps2 & PS2_BTN_CROSS)    psp |= PSP_CTRL_DOWN;
		if(ps2 & PS2_BTN_SQUARE)   psp |= PSP_CTRL_LEFT;
	}
	else
	{
		if(ps2 & PS2_BTN_TRIANGLE) psp |= PSP_CTRL_TRIANGLE;
		if(ps2 & PS2_BTN_CIRCLE)   psp |= PSP_CTRL_CIRCLE;
		if(ps2 & PS2_BTN_CROSS)    psp |= PSP_CTRL_CROSS;
		if(ps2 & PS2_BTN_SQUARE)   psp |= PSP_CTRL_SQUARE;
	}

	return psp;
}

static uint32_t ps2_right_stick_to_dpad(uint8_t rx, uint8_t ry)
{
	uint32_t buttons = 0;

	if(rx < PS2_STICK_LOW)
	{
		buttons |= PSP_CTRL_LEFT;
	}
	else if(rx > PS2_STICK_HIGH)
	{
		buttons |= PSP_CTRL_RIGHT;
	}

	if(ry < PS2_STICK_LOW)
	{
		buttons |= PSP_CTRL_UP;
	}
	else if(ry > PS2_STICK_HIGH)
	{
		buttons |= PSP_CTRL_DOWN;
	}

	return buttons;
}

static uint32_t ps2_right_stick_to_face(uint8_t rx, uint8_t ry)
{
	uint32_t buttons = 0;

	if(rx < PS2_FACE_SWAP_STICK_LOW)
	{
		buttons |= PSP_CTRL_SQUARE;
	}
	else if(rx > PS2_FACE_SWAP_STICK_HIGH)
	{
		buttons |= PSP_CTRL_CIRCLE;
	}

	if(ry < PS2_FACE_SWAP_STICK_LOW)
	{
		buttons |= PSP_CTRL_TRIANGLE;
	}
	else if(ry > PS2_FACE_SWAP_STICK_HIGH)
	{
		buttons |= PSP_CTRL_CROSS;
	}

	return buttons;
}

static void ps2_rotate_left_analog_for_star_soldier(uint8_t in_x, uint8_t in_y,
	uint8_t *out_x, uint8_t *out_y);
static void update_right_stick_mode_toggle(uint32_t ps2_buttons);

static void clear_controller_state_value(struct ControllerState *state)
{
	state->buttons = 0;
	state->x = 128;
	state->y = 128;
}

static void ps2_apply_standard_mapping(const uint8_t *rx,
	struct ControllerState *state, uint32_t ps2_buttons, int use_global_modes)
{
	state->buttons = ps2_buttons_to_psp(ps2_buttons);

	if(!ps2_is_analog_mode(rx[PS2_RESP_MODE]))
	{
		return;
	}

	if(use_global_modes)
	{
		g_ps2_analog_retry_start_ms = 0;
	}
	if(use_global_modes && g_right_stick_mode == RIGHT_STICK_MODE_DPAD)
	{
		state->buttons |= ps2_right_stick_to_dpad(
			rx[PS2_RESP_RIGHT_X], rx[PS2_RESP_RIGHT_Y]);
	}
	else if(use_global_modes && g_right_stick_mode == RIGHT_STICK_MODE_FACE_SWAP)
	{
		state->buttons |= ps2_right_stick_to_face(
			rx[PS2_RESP_RIGHT_X], rx[PS2_RESP_RIGHT_Y]);
	}
	if(use_global_modes && g_right_stick_mode == RIGHT_STICK_MODE_STAR_SOLDIER)
	{
		ps2_rotate_left_analog_for_star_soldier(
			rx[PS2_RESP_LEFT_X], rx[PS2_RESP_LEFT_Y],
			&state->x, &state->y);
	}
	else
	{
		state->x = rx[PS2_RESP_LEFT_X];
		state->y = rx[PS2_RESP_LEFT_Y];
	}
}

static int ps2_decode_poll_response(const uint8_t *rx,
	struct ControllerState *state, uint32_t *ps2_buttons, int use_global_modes)
{
	clear_controller_state_value(state);

	if(rx[PS2_RESP_MODE] == 0x00 || rx[PS2_RESP_MODE] == 0xFF ||
		rx[PS2_RESP_ID] != 0x5A)
	{
		*ps2_buttons = 0;
		return 0;
	}

	*ps2_buttons = (~(((uint32_t) rx[PS2_RESP_BUTTON_HI] << 8) |
		rx[PS2_RESP_BUTTON_LO])) & 0xFFFFUL;

	if(use_global_modes)
	{
		update_popn_music_controller_mode(*ps2_buttons);
		if(g_popn_music_controller_mode)
		{
			state->buttons = ps2_popn_music_buttons_to_psp(*ps2_buttons);
			return 1;
		}

		update_right_stick_mode_toggle(*ps2_buttons);
	}

	ps2_apply_standard_mapping(rx, state, *ps2_buttons, use_global_modes);
	return 1;
}

static int ps2_decode_multitap_long_slot(const uint8_t *slot,
	struct ControllerState *state, uint32_t *ps2_buttons, int use_global_modes)
{
	uint8_t rx[PS2_RESPONSE_SIZE] = {
		0xFF, slot[0], slot[1], slot[2], slot[3], slot[4], slot[5], slot[6], slot[7]
	};

	return ps2_decode_poll_response(rx, state, ps2_buttons, use_global_modes);
}

static int ps2_poll_multitap_long_slots(struct ControllerState *state1,
	uint32_t *ps2_buttons1, uint8_t *mode1, struct ControllerState *state2,
	uint32_t *ps2_buttons2, uint8_t *mode2)
{
	uint8_t request_tx[PS2_RESPONSE_SIZE] = {
		0x01, 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	uint8_t request_rx[PS2_RESPONSE_SIZE];
	uint8_t long_tx[PS2_MULTITAP_LONG_RESPONSE_SIZE];
	uint8_t long_rx[PS2_MULTITAP_LONG_RESPONSE_SIZE];
	const uint8_t *slot1;
	const uint8_t *slot2;
	int slot1_present;
	int slot2_present;
	uint32_t now = to_ms_since_boot(get_absolute_time());

	*ps2_buttons1 = 0;
	*ps2_buttons2 = 0;
	*mode1 = 0;
	*mode2 = 0;
	clear_controller_state_value(state1);
	clear_controller_state_value(state2);

	if(!g_ps2_multitap_long_active &&
		(uint32_t) (now - g_ps2_multitap_long_probe_last_ms) <
			PS2_MULTITAP_LONG_PROBE_INTERVAL_MS)
	{
		return 0;
	}
	g_ps2_multitap_long_probe_last_ms = now;

	memset(request_rx, 0, sizeof(request_rx));
	ps2_transaction(request_tx, request_rx, sizeof(request_tx));

	memset(long_tx, 0, sizeof(long_tx));
	long_tx[0] = 0x01;
	long_tx[1] = 0x42;
	long_tx[2] = 0x00;
	memset(long_rx, 0, sizeof(long_rx));
	ps2_transaction(long_tx, long_rx, sizeof(long_rx));

	if(long_rx[1] != 0x80 || long_rx[2] != 0x5A)
	{
		g_ps2_multitap_long_active = 0;
		return 0;
	}

	if(!g_ps2_multitap_long_active)
	{
		printf("PS2 multitap long response detected\n");
		g_ps2_multitap_long_active = 1;
	}

	slot1 = &long_rx[3];
	slot2 = &long_rx[3 + PS2_MULTITAP_LONG_SLOT_SIZE];
	slot1_present = ps2_decode_multitap_long_slot(slot1, state1, ps2_buttons1, 1);
	slot2_present = ps2_decode_multitap_long_slot(slot2, state2, ps2_buttons2, 0);
	*mode1 = slot1[0];
	*mode2 = slot2[0];

	return slot1_present || slot2_present;
}

static void ps2_log_slot_state(int slot, int present,
	const struct ControllerState *state, uint32_t ps2_buttons, uint8_t mode)
{
	int index = slot - 1;

	if(index < 0 || index >= PS2_MULTITAP_SLOT_COUNT)
	{
		return;
	}

	if(present != g_ps2_slot_present[index])
	{
		g_ps2_slot_present[index] = present;
		if(present)
		{
			printf("PS2 multitap slot %d connected, mode 0x%02X\n", slot, mode);
		}
		else
		{
			printf("PS2 multitap slot %d disconnected\n", slot);
		}
	}

	if(!present)
	{
		g_ps2_slot_last_buttons[index] = 0;
		g_ps2_slot_last_x[index] = 128;
		g_ps2_slot_last_y[index] = 128;
		return;
	}

	if(ps2_buttons != g_ps2_slot_last_buttons[index] ||
		abs_int((int) state->x - g_ps2_slot_last_x[index]) >= ANALOG_DEADBAND ||
		abs_int((int) state->y - g_ps2_slot_last_y[index]) >= ANALOG_DEADBAND)
	{
		printf("PS2 multitap slot %d state: raw 0x%04lX psp 0x%06lX lx %u ly %u\n",
			slot, (unsigned long) ps2_buttons, (unsigned long) state->buttons,
			state->x, state->y);
		g_ps2_slot_last_buttons[index] = ps2_buttons;
		g_ps2_slot_last_x[index] = state->x;
		g_ps2_slot_last_y[index] = state->y;
	}
}

static void update_p2_state(int present, const struct ControllerState *state)
{
	g_p2_present = present;
	if(present)
	{
		g_p2_state = *state;
	}
	else
	{
		clear_controller_state_value(&g_p2_state);
	}
}

static void ps2_rotate_left_analog_for_star_soldier(uint8_t in_x, uint8_t in_y,
	uint8_t *out_x, uint8_t *out_y)
{
	*out_x = (uint8_t) clamp_int(PS2_ANALOG_CENTER +
		(PS2_ANALOG_CENTER - (int) in_y), 0, 255);
	*out_y = in_x;
}

static const char *right_stick_mode_name(int mode)
{
	switch(mode)
	{
		case RIGHT_STICK_MODE_DPAD:
			return "right stick to D-pad";
		case RIGHT_STICK_MODE_FACE_SWAP:
			return "right stick to face buttons, face buttons to D-pad";
		case RIGHT_STICK_MODE_STAR_SOLDIER:
			return "Star Soldier D-pad rotate";
		default:
			return "off";
	}
}

static void update_right_stick_mode_toggle(uint32_t ps2_buttons)
{
	int r3_pressed = (ps2_buttons & PS2_BTN_R3) != 0;

	if((ps2_buttons & PS2_BTN_START) && r3_pressed && !g_last_ps2_r3_pressed)
	{
		g_right_stick_mode = (g_right_stick_mode + 1) % RIGHT_STICK_MODE_COUNT;
		printf("Right stick mode: %s\n", right_stick_mode_name(g_right_stick_mode));
		update_status_led();
	}

	g_last_ps2_r3_pressed = r3_pressed;
}

static int read_ps2_controller_state(struct ControllerState *state)
{
	uint8_t rx[PS2_RESPONSE_SIZE];
	uint8_t rx2[PS2_RESPONSE_SIZE];
	uint32_t ps2_buttons;
	uint32_t ps2_buttons2;
	struct ControllerState state2;
	int slot2_present;
	uint8_t slot2_mode;
	uint8_t slot1_mode;

	clear_controller_state_value(state);

	if(!ps2_poll_raw(rx))
	{
		clear_controller_state_value(&state2);
		slot1_mode = 0;
		slot2_mode = 0;
		if(ps2_poll_multitap_long_slots(state, &ps2_buttons, &slot1_mode,
			&state2, &ps2_buttons2, &slot2_mode))
		{
			int slot1_present = ps2_buttons != 0 ||
				state->buttons != 0 || slot1_mode != 0;
			int slot2_present = ps2_buttons2 != 0 ||
				state2.buttons != 0 || slot2_mode != 0;

			ps2_log_slot_state(1, slot1_present, state, ps2_buttons, slot1_mode);
			ps2_log_slot_state(2, slot2_present, &state2, ps2_buttons2, slot2_mode);
			update_p2_state(slot2_present, &state2);
			return slot1_present || slot2_present;
		}

		update_popn_music_controller_mode(0);
		ps2_log_slot_state(1, 0, state, 0, 0);
		ps2_log_slot_state(2, 0, &state2, 0, 0);
		update_p2_state(0, &state2);
		return 0;
	}

	if(!ps2_decode_poll_response(rx, state, &ps2_buttons, 1))
	{
		ps2_log_slot_state(1, 0, state, 0, 0);
		return 0;
	}
	ps2_log_slot_state(1, 1, state, ps2_buttons, rx[PS2_RESP_MODE]);

	clear_controller_state_value(&state2);
	slot2_mode = 0;
	slot2_present = ps2_poll_raw_addr(0x02, rx2) &&
		ps2_decode_poll_response(rx2, &state2, &ps2_buttons2, 0);
	if(slot2_present)
	{
		slot2_mode = rx2[PS2_RESP_MODE];
	}
	else
	{
		struct ControllerState long_state1;
		uint32_t long_buttons1;
		uint8_t long_mode1;

		slot2_present = ps2_poll_multitap_long_slots(&long_state1,
			&long_buttons1, &long_mode1, &state2, &ps2_buttons2, &slot2_mode) &&
			(ps2_buttons2 != 0 || state2.buttons != 0 || slot2_mode != 0);
	}
	ps2_log_slot_state(2, slot2_present, &state2,
		slot2_present ? ps2_buttons2 : 0,
		slot2_present ? slot2_mode : 0);
	update_p2_state(slot2_present, &state2);

	return 1;
}

static void init_ps2_pins(void)
{
	gpio_init(PS2_PIN_ATT);
	gpio_set_dir(PS2_PIN_ATT, GPIO_OUT);
	gpio_put(PS2_PIN_ATT, 1);

	gpio_init(PS2_PIN_CMD);
	gpio_set_dir(PS2_PIN_CMD, GPIO_OUT);
	gpio_put(PS2_PIN_CMD, 1);

	gpio_init(PS2_PIN_CLK);
	gpio_set_dir(PS2_PIN_CLK, GPIO_OUT);
	gpio_put(PS2_PIN_CLK, 1);

	gpio_init(PS2_PIN_DAT);
	gpio_set_dir(PS2_PIN_DAT, GPIO_IN);
	gpio_pull_up(PS2_PIN_DAT);

#if PS2_USE_ACK
	gpio_init(PS2_PIN_ACK);
	gpio_set_dir(PS2_PIN_ACK, GPIO_IN);
	gpio_pull_up(PS2_PIN_ACK);
#endif

	sleep_ms(300);
	ps2_enable_analog_mode();
	ps2_enable_analog_mode_addr(0x02);
	ps2_start_analog_retry();
	printf("PS2 bitbang input initialized: ATT GP%d CMD GP%d DAT GP%d CLK GP%d\n",
		PS2_PIN_ATT, PS2_PIN_CMD, PS2_PIN_DAT, PS2_PIN_CLK);
}

static void init_controller_input(void)
{
	init_input_pins();
#if INPUT_SOURCE == INPUT_SOURCE_PS2
	init_ps2_pins();
#endif
}

static int read_controller_state(struct ControllerState *state)
{
#if INPUT_SOURCE == INPUT_SOURCE_PS2
	ps2_retry_analog_mode();
	if(read_ps2_controller_state(state))
	{
		return 1;
	}

	state->buttons = 0;
	state->x = 128;
	state->y = 128;
	return 1;
#else
	read_gpio_controller_state(state);
	return 1;
#endif
}

static int event_queue_is_empty(void)
{
	return g_event_head == g_event_tail;
}

static int event_queue_is_full(void)
{
	return ((g_event_tail + 1) % EVENT_QUEUE_SIZE) == g_event_head;
}

static int is_button_event_type(int32_t type)
{
	return type == RJM_TYPE_BUTTON_DOWN || type == RJM_TYPE_BUTTON_UP ||
		type == RJM_TYPE_P2_BUTTON_DOWN || type == RJM_TYPE_P2_BUTTON_UP;
}

static int enqueue_joy_event(int32_t type, uint32_t value)
{
	if(value == 0 && is_button_event_type(type))
	{
		return 1;
	}

	if(event_queue_is_full())
	{
		printf("JoyEvent queue full, dropping type %ld value 0x%06lX\n",
			(long) type, (unsigned long) value);
		return 0;
	}

	g_event_queue[g_event_tail].type = type;
	g_event_queue[g_event_tail].value = value;
	g_event_tail = (g_event_tail + 1) % EVENT_QUEUE_SIZE;
	return 1;
}

static int send_joy_event_now(int32_t type, uint32_t value)
{
	size_t len;

	len = rjm_build_async_joy_event(g_xfer_buf, type, value);
#if ENABLE_JOY_EVENT_TRACE
	printf("Sending JoyEvent type %ld value 0x%06lX to endpoint 0x%02X\n",
		(long) type, (unsigned long) value, g_eps.ep_bulk_out_async);
#endif
	return submit_xfer(g_eps.ep_bulk_out_async, g_xfer_buf, (uint16_t) len, joy_event_sent_cb);
}

static void process_event_queue(void)
{
	struct JoyEventQueueItem item;

	if(!g_handshake_complete || g_async_xfer_busy || event_queue_is_empty())
	{
		return;
	}

	item = g_event_queue[g_event_head];
	if(send_joy_event_now(item.type, item.value))
	{
		g_async_xfer_busy = 1;
		g_event_head = (g_event_head + 1) % EVENT_QUEUE_SIZE;
	}
	else
	{
		printf("Failed to submit JoyEvent transfer\n");
	}
}

static void poll_input_state(void)
{
	uint32_t now;
	uint32_t changed;
	uint32_t p2_changed;
	struct ControllerState state;
	struct ControllerState p2_state;

	if(!g_input_started)
	{
		return;
	}

	now = to_ms_since_boot(get_absolute_time());
	if((uint32_t) (now - g_last_poll_ms) < STATE_REPORT_MS)
	{
		return;
	}
	g_last_poll_ms = now;

	if(!read_controller_state(&state))
	{
		return;
	}

	p2_state = g_p2_state;

	changed = state.buttons ^ g_last_buttons;
	if(changed != 0)
	{
		enqueue_joy_event(RJM_TYPE_BUTTON_DOWN, changed & state.buttons);
		enqueue_joy_event(RJM_TYPE_BUTTON_UP, changed & g_last_buttons);
		g_last_buttons = state.buttons;
	}

	if(abs_int((int) state.x - g_last_x) >= ANALOG_DEADBAND)
	{
		enqueue_joy_event(RJM_TYPE_ANALOG_X, state.x);
		g_last_x = state.x;
	}
	if(abs_int((int) state.y - g_last_y) >= ANALOG_DEADBAND)
	{
		enqueue_joy_event(RJM_TYPE_ANALOG_Y, state.y);
		g_last_y = state.y;
	}

	if(g_p2_present != g_last_p2_present)
	{
		enqueue_joy_event(RJM_TYPE_P2_STATUS, g_p2_present ? 1 : 0);
		g_last_p2_present = g_p2_present;
	}

	p2_changed = p2_state.buttons ^ g_last_p2_buttons;
	if(p2_changed != 0)
	{
		enqueue_joy_event(RJM_TYPE_P2_BUTTON_DOWN, p2_changed & p2_state.buttons);
		enqueue_joy_event(RJM_TYPE_P2_BUTTON_UP, p2_changed & g_last_p2_buttons);
		g_last_p2_buttons = p2_state.buttons;
	}

	if(abs_int((int) p2_state.x - g_last_p2_x) >= ANALOG_DEADBAND)
	{
		enqueue_joy_event(RJM_TYPE_P2_ANALOG_X, p2_state.x);
		g_last_p2_x = p2_state.x;
	}
	if(abs_int((int) p2_state.y - g_last_p2_y) >= ANALOG_DEADBAND)
	{
		enqueue_joy_event(RJM_TYPE_P2_ANALOG_Y, p2_state.y);
		g_last_p2_y = p2_state.y;
	}

	if((uint32_t) (now - g_last_button_reassert_ms) >= BUTTON_REASSERT_MS)
	{
		g_last_button_reassert_ms = now;
		if(state.buttons != 0)
		{
			enqueue_joy_event(RJM_TYPE_BUTTON_DOWN, state.buttons);
		}
		if(g_p2_present && p2_state.buttons != 0)
		{
			enqueue_joy_event(RJM_TYPE_P2_BUTTON_DOWN, p2_state.buttons);
		}
	}

#if STATE_HEARTBEAT_MS > 0
	if((uint32_t) (now - g_last_heartbeat_ms) >= STATE_HEARTBEAT_MS)
	{
		g_last_heartbeat_ms = now;
		if(state.buttons != 0)
		{
			enqueue_joy_event(RJM_TYPE_BUTTON_DOWN, state.buttons);
		}
		enqueue_joy_event(RJM_TYPE_ANALOG_X, state.x);
		enqueue_joy_event(RJM_TYPE_ANALOG_Y, state.y);
	}
#endif

	process_event_queue();
}

static void poll_handshake_timeout(void)
{
	uint32_t now;

	if(!g_handshake_started || g_handshake_complete)
	{
		return;
	}

	now = to_ms_since_boot(get_absolute_time());
	if(g_sending_magic)
	{
		if((uint32_t) (now - g_handshake_last_ms) < HOSTFS_MAGIC_TIMEOUT_MS)
		{
			return;
		}

		printf("HOSTFS magic write timed out; waiting for USB reconnect\n");
		g_sending_magic = 0;
		g_handshake_started = 0;
		return;
	}

	if(!g_waiting_for_hello)
	{
		return;
	}

	if((uint32_t) (now - g_handshake_last_ms) < HOSTFS_HELLO_TIMEOUT_MS)
	{
		return;
	}

	printf("HostFS hello timed out; aborting read and retrying magic\n");
	tuh_edpt_abort_xfer(g_daddr, g_eps.ep_bulk_in);
	g_waiting_for_hello = 0;
	send_hostfs_magic();
}

static void start_input_scan(void)
{
	struct ControllerState state;

#if ENABLE_INPUT_SCAN
	if(g_input_started)
	{
		return;
	}

	g_input_started = 1;
	reset_input_diff_state();
	if(read_controller_state(&state))
	{
		g_last_buttons = state.buttons;
		g_last_x = state.x;
		g_last_y = state.y;
		g_last_p2_buttons = g_p2_state.buttons;
		g_last_p2_x = g_p2_state.x;
		g_last_p2_y = g_p2_state.y;
	}
#if INPUT_SOURCE == INPUT_SOURCE_PS2
	printf("Starting RP2040 input scanner. PS2 bitbang input now drives RemoteJoyMinus.\n");
#else
	printf("Starting RP2040 input scanner. GPIO buttons and analog axes now drive RemoteJoyMinus.\n");
#endif
#else
	(void) state;
	printf("Input scanner disabled for handshake-only test.\n");
#endif
}

static void joy_event_sent_cb(tuh_xfer_t *xfer)
{
	g_async_xfer_busy = 0;
	if(xfer->result != XFER_RESULT_SUCCESS)
	{
		printf("JoyEvent transfer failed, result %d\n", xfer->result);
		return;
	}

#if ENABLE_JOY_EVENT_TRACE
	printf("JoyEvent sent (%lu bytes)\n", (unsigned long) xfer->actual_len);
#endif
	process_event_queue();
}

static void hostfs_magic_sent_cb(tuh_xfer_t *xfer)
{
	g_sending_magic = 0;
	if(xfer->result != XFER_RESULT_SUCCESS)
	{
		printf("HOSTFS magic write failed, result %d\n", xfer->result);
		return;
	}

	printf("HOSTFS magic sent (%lu bytes), waiting for hello command on 0x%02X\n",
		(unsigned long) xfer->actual_len, g_eps.ep_bulk_in);
	memset(g_xfer_buf, 0, sizeof(g_xfer_buf));
	g_waiting_for_hello = 1;
	g_handshake_last_ms = to_ms_since_boot(get_absolute_time());
	if(!submit_xfer(g_eps.ep_bulk_in, g_xfer_buf, HOSTFS_CMD_SIZE, hostfs_hello_read_cb))
	{
		g_waiting_for_hello = 0;
		printf("Failed to submit HostFS hello read\n");
	}
}

static void hostfs_hello_read_cb(tuh_xfer_t *xfer)
{
	uint32_t magic;
	uint32_t command;
	uint32_t extralen;
	size_t len;

	if(xfer->result != XFER_RESULT_SUCCESS)
	{
		g_waiting_for_hello = 0;
		printf("HostFS hello read failed, result %d\n", xfer->result);
		return;
	}
	if(xfer->actual_len < HOSTFS_CMD_SIZE)
	{
		g_waiting_for_hello = 0;
		printf("HostFS hello too short: %lu bytes\n", (unsigned long) xfer->actual_len);
		return;
	}

	g_waiting_for_hello = 0;

	magic = le32(&g_xfer_buf[0]);
	command = le32(&g_xfer_buf[4]);
	extralen = le32(&g_xfer_buf[8]);
	printf("HostFS hello command: magic 0x%08lX command 0x%08lX extralen %lu\n",
		(unsigned long) magic, (unsigned long) command, (unsigned long) extralen);

	if(magic != RJM_HOSTFS_MAGIC || command != RJM_HOSTFS_CMD_HELLO || extralen != 0)
	{
		printf("Unexpected HostFS hello command\n");
		return;
	}

	len = rjm_build_hello_response(g_xfer_buf);
	printf("Sending HostFS hello response to endpoint 0x%02X\n", g_eps.ep_bulk_out_cmd);
	if(!submit_xfer(g_eps.ep_bulk_out_cmd, g_xfer_buf, (uint16_t) len, hostfs_hello_response_sent_cb))
	{
		printf("Failed to submit HostFS hello response\n");
	}
}

static void hostfs_hello_response_sent_cb(tuh_xfer_t *xfer)
{
	if(xfer->result != XFER_RESULT_SUCCESS)
	{
		printf("HostFS hello response write failed, result %d\n", xfer->result);
		return;
	}

	g_handshake_complete = 1;
	reset_input_diff_state();
#if INPUT_SOURCE == INPUT_SOURCE_PS2
	ps2_start_analog_retry();
#endif
	update_status_led();
	printf("HostFS hello handshake complete (%lu bytes). remotejoy-minus.prx should now be connected.\n",
		(unsigned long) xfer->actual_len);
	printf("Ready to send RemoteJoyMinus JoyEvent packets to endpoint 0x%02X.\n", g_eps.ep_bulk_out_async);
	start_input_scan();
}

static void parse_config_descriptor(uint8_t daddr, const uint8_t *desc, uint16_t len)
{
	uint16_t offset = 0;
	int in_target_interface = 0;
	int target_found = 0;

	clear_endpoints();

	printf("Config descriptor len %u\n", len);

	while(offset + 2 <= len)
	{
		uint8_t dlen = desc[offset];
		uint8_t dtype = desc[offset + 1];
		const uint8_t *p = &desc[offset];

		if(dlen < 2 || offset + dlen > len)
		{
			printf("Descriptor parse stopped at offset %u\n", offset);
			break;
		}

		if(dtype == TUSB_DESC_INTERFACE && dlen >= 9)
		{
			in_target_interface = 0;
			printf("  interface %u alt %u eps %u class %02X/%02X/%02X\n",
				p[2], p[3], p[4], p[5], p[6], p[7]);

			if(p[5] == 0xFF && p[6] == 0x01 && p[7] == 0xFF)
			{
				in_target_interface = 1;
				target_found = 1;
				g_eps.interface_number = p[2];
				printf("  -> USBHostFS candidate interface\n");
			}
		}
		else if(in_target_interface && is_bulk_endpoint(p) && dlen >= 7)
		{
			uint8_t addr = p[2];
			uint16_t mps = le16(&p[4]);

			print_endpoint(p);
			if(addr & 0x80)
			{
				g_eps.ep_bulk_in = addr;
				g_eps.bulk_in_mps = mps;
				save_endpoint_desc(&g_ep_bulk_in_desc, p);
			}
			else if(g_eps.ep_bulk_out_cmd == 0)
			{
				g_eps.ep_bulk_out_cmd = addr;
				g_eps.bulk_out_cmd_mps = mps;
				save_endpoint_desc(&g_ep_bulk_out_cmd_desc, p);
			}
			else if(g_eps.ep_bulk_out_async == 0)
			{
				g_eps.ep_bulk_out_async = addr;
				g_eps.bulk_out_async_mps = mps;
				save_endpoint_desc(&g_ep_bulk_out_async_desc, p);
			}
		}

		offset += dlen;
	}

	if(!target_found)
	{
		printf("USBHostFS interface not found on device %u\n", daddr);
		return;
	}

	printf("USBHostFS endpoints on device %u:\n", daddr);
	printf("  interface       %u\n", g_eps.interface_number);
	printf("  bulk in         0x%02X mps %u\n", g_eps.ep_bulk_in, g_eps.bulk_in_mps);
	printf("  bulk out cmd    0x%02X mps %u\n", g_eps.ep_bulk_out_cmd, g_eps.bulk_out_cmd_mps);
	printf("  bulk out async  0x%02X mps %u\n", g_eps.ep_bulk_out_async, g_eps.bulk_out_async_mps);

	if(g_eps.ep_bulk_in == 0x81 && g_eps.ep_bulk_out_cmd == 0x02 && g_eps.ep_bulk_out_async == 0x03)
	{
		printf("Endpoint layout matches USBHostFS notes. Ready for hello handshake step.\n");
	}
	else
	{
		printf("Endpoint layout differs from expected notes; use discovered values for next step.\n");
	}

	start_hostfs_hello(daddr);
}

static void config_desc_cb(tuh_xfer_t *xfer)
{
	uint16_t total_len;

	if(xfer->result != XFER_RESULT_SUCCESS)
	{
		printf("Failed to read config descriptor, result %d\n", xfer->result);
		return;
	}

	total_len = le16(&g_desc_config[2]);
	if(total_len > xfer->actual_len)
	{
		total_len = xfer->actual_len;
	}

	parse_config_descriptor(xfer->daddr, g_desc_config, total_len);
}

static void device_desc_cb(tuh_xfer_t *xfer)
{
	uint16_t vid;
	uint16_t pid;

	if(xfer->result != XFER_RESULT_SUCCESS)
	{
		printf("Failed to read device descriptor, result %d\n", xfer->result);
		return;
	}

	vid = le16(&g_desc_device[8]);
	pid = le16(&g_desc_device[10]);
	printf("Device descriptor: vid %04X pid %04X class %02X/%02X/%02X maxpkt0 %u\n",
		vid, pid, g_desc_device[4], g_desc_device[5], g_desc_device[6], g_desc_device[7]);

	if(!tuh_descriptor_get_configuration(xfer->daddr, 0, g_desc_config, sizeof(g_desc_config), config_desc_cb, 0))
	{
		printf("Could not request config descriptor\n");
	}
}

void tuh_mount_cb(uint8_t daddr)
{
	printf("Device mounted: address %u\n", daddr);
	clear_endpoints();

	if(!tuh_descriptor_get_device(daddr, g_desc_device, sizeof(g_desc_device), device_desc_cb, 0))
	{
		printf("Could not request device descriptor\n");
	}
}

void tuh_umount_cb(uint8_t daddr)
{
	printf("Device unmounted: address %u\n", daddr);
	clear_endpoints();
}

int main(void)
{
	stdio_init_all();
	board_init();
	init_status_led();
	init_controller_input();
	tusb_init();
	clear_endpoints();

	printf("\nRemoteJoyMinus standalone USB host\n");
	printf("HOSTFS magic 0x%08lX ASYNC magic 0x%08lX channel %d\n",
		(unsigned long) RJM_HOSTFS_MAGIC,
		(unsigned long) RJM_ASYNC_MAGIC,
		RJM_ASYNC_JOY_CHANNEL);
	printf("Waiting for PSP USBHostFS device...\n");

	while(1)
	{
		tuh_task();
		poll_handshake_timeout();
		poll_input_state();
		process_event_queue();
	}
}

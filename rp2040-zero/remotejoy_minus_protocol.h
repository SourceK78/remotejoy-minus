/*
 * remotejoy_minus_protocol.h - Shared protocol constants for RP2040 senders
 */
#ifndef REMOTEJOY_MINUS_PROTOCOL_H
#define REMOTEJOY_MINUS_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define RJM_HOSTFS_MAGIC 0x782F0812UL
#define RJM_ASYNC_MAGIC  0x782F0813UL
#define RJM_JOY_MAGIC    0x909ACCEFUL

#define RJM_HOSTFS_CMD_HELLO 0x8FFC0000UL
#define RJM_ASYNC_JOY_CHANNEL 4

#define RJM_TYPE_BUTTON_DOWN 1
#define RJM_TYPE_BUTTON_UP   2
#define RJM_TYPE_ANALOG_Y    3
#define RJM_TYPE_ANALOG_X    4
#define RJM_TYPE_P2_BUTTON_DOWN 6
#define RJM_TYPE_P2_BUTTON_UP   7
#define RJM_TYPE_P2_ANALOG_Y    8
#define RJM_TYPE_P2_ANALOG_X    9
#define RJM_TYPE_P2_STATUS      10

#define PSP_CTRL_SELECT     0x000001UL
#define PSP_CTRL_START      0x000008UL
#define PSP_CTRL_UP         0x000010UL
#define PSP_CTRL_RIGHT      0x000020UL
#define PSP_CTRL_DOWN       0x000040UL
#define PSP_CTRL_LEFT       0x000080UL
#define PSP_CTRL_LTRIGGER   0x000100UL
#define PSP_CTRL_RTRIGGER   0x000200UL
#define PSP_CTRL_TRIANGLE   0x001000UL
#define PSP_CTRL_CIRCLE     0x002000UL
#define PSP_CTRL_CROSS      0x004000UL
#define PSP_CTRL_SQUARE     0x008000UL
#define PSP_CTRL_HOME       0x010000UL
#define PSP_CTRL_VOLUP      0x100000UL
#define PSP_CTRL_VOLDOWN    0x200000UL
#define PSP_CTRL_NOTE       0x800000UL

struct RjmHostFsCmd
{
	uint32_t magic;
	uint32_t command;
	uint32_t extralen;
} __attribute__((packed));

struct RjmAsyncCommand
{
	uint32_t magic;
	uint32_t channel;
} __attribute__((packed));

struct RjmJoyEvent
{
	uint32_t magic;
	int32_t type;
	uint32_t value;
} __attribute__((packed));

static inline void rjm_write_le32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t) (value & 0xFF);
	out[1] = (uint8_t) ((value >> 8) & 0xFF);
	out[2] = (uint8_t) ((value >> 16) & 0xFF);
	out[3] = (uint8_t) ((value >> 24) & 0xFF);
}

static inline size_t rjm_build_hostfs_magic(uint8_t *out)
{
	rjm_write_le32(out, RJM_HOSTFS_MAGIC);
	return 4;
}

static inline size_t rjm_build_hello_response(uint8_t *out)
{
	rjm_write_le32(out + 0, RJM_HOSTFS_MAGIC);
	rjm_write_le32(out + 4, RJM_HOSTFS_CMD_HELLO);
	rjm_write_le32(out + 8, 0);
	return 12;
}

static inline size_t rjm_build_async_joy_event(uint8_t *out, int32_t type, uint32_t value)
{
	rjm_write_le32(out + 0, RJM_ASYNC_MAGIC);
	rjm_write_le32(out + 4, RJM_ASYNC_JOY_CHANNEL);
	rjm_write_le32(out + 8, RJM_JOY_MAGIC);
	rjm_write_le32(out + 12, (uint32_t) type);
	rjm_write_le32(out + 16, value);
	return 20;
}

#endif

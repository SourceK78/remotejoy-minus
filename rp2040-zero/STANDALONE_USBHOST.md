RemoteJoyMinus Standalone USB Host Notes
========================================

This note captures the smallest PSP USBHostFS behavior needed for a future
RP2040-zero standalone sender. It is derived from usbhostfs/main.c and
usbhostfs_pc/main.c in this repository.

Goal
----

Replace this older chain:

	RP2040-zero serial -> rjminus-serial-bridge -> usbhostfs_pc -> PSP

with:

	RP2040-zero USB host -> PSP

The RP2040 host only needs to deliver RemoteJoyMinus JoyEvent packets to
the RemoteJoyMinus async input channel.

Hardware Notes
--------------

The RP2040-zero must act as the USB host for the PSP. That means:

	5V VBUS must be supplied to the PSP USB connector.
	GND, D+ and D- must be wired correctly.
	The same RP2040 USB port cannot simultaneously be normal USB serial to the PC.
	Use UART pins or another debug path for logs during standalone testing.

Be careful with VBUS. Do not connect two USB hosts to the same USB bus.

USB Interface
-------------

remotejoy-minus.prx exposes a vendor-specific interface compatible with the
minimal RemoteJoy/usbhostfs wire layout:

	interface class     0xFF
	interface subclass  0x01
	interface protocol  0xFF
	configuration       1

Full-speed endpoint layout:

	0x81  bulk IN   PSP -> host
	0x02  bulk OUT  host -> PSP command/response
	0x03  bulk OUT  host -> PSP async input

RP2040 full-speed host should use max packet size 64.

Minimum Handshake
-----------------

1. Enumerate the PSP USBHostFS device and set configuration 1.

2. Write the four-byte little-endian HOSTFS magic to endpoint 0x02:

	0x782F0812

3. Read a HostFsHelloCmd from endpoint 0x81:

	struct HostFsCmd {
		u32 magic;     // 0x782F0812
		u32 command;   // 0x8FFC0000, HOSTFS_CMD_HELLO
		u32 extralen;  // 0
	}

4. Write a matching HostFsHelloResp to endpoint 0x02:

	magic    = 0x782F0812
	command  = 0x8FFC0000
	extralen = 0

After this succeeds, remotejoy-minus.prx marks the host as connected and starts
an async receive request on endpoint 0x03.

Sending Input
-------------

Each RemoteJoyMinus input event is sent to endpoint 0x03 as:

	struct AsyncCommand {
		u32 magic;    // 0x782F0813
		u32 channel;  // 4
	}

	struct JoyEvent {
		u32 magic;    // 0x909ACCEF
		s32 type;
		u32 value;
	}

Total packet size is 20 bytes.

JoyEvent types:

	1  button down
	2  button up
	3  analog Y
	4  analog X

The Arduino serial sketch currently sends a complete state line every 20 ms.
The standalone sender should keep that same state model internally, diff the
button mask against the previous state, and emit these JoyEvent packets:

	buttons newly set      -> type 1, value changed_mask
	buttons newly cleared  -> type 2, value changed_mask
	X changed              -> type 4, value 0..255
	Y changed              -> type 3, value 0..255

Shared Constants
----------------

remotejoy_minus_protocol.h contains the protocol constants and packet builders
for this standalone host implementation:

	remotejoy_minus_protocol.h

Open Questions
--------------

The minimal path above should be enough for RemoteJoyMinus input only. If the
PSP side starts using normal HostFS file commands or shell/stdout channels in
the same session, the RP2040 host will need to handle or safely reject more of
the HostFS protocol.

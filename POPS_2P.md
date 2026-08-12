POPS 2P Support Notes
=====================

Overview
--------

remotejoy-minus can optionally provide a second controller to POPS. The
RP2040-zero firmware obtains it from a compatible PS1 multitap; the Pico 2 W
firmware obtains it from its persistent Bluetooth 2P slot.

The intended behavior is:

	Input Slot 1  -> normal remotejoy-minus input
	Input Slot 2  -> POPS player-2 input

Slot 1 is deliberately kept as the normal remotejoy-minus input stream. It is
not forced to POPS player 1. This means POPS's own HOME-menu controller
assignment continues to decide how PSP hardware input and the normal remote
input are handled.

Slot 2 is sent over the remotejoy-minus async channel as a separate player-2
event stream. The PSP plugin enables the POPS 2P patch only while Slot 2 is
connected.

Build
-----

The PSP plugin must be built with POPS 2P enabled:

	make clean
	make RJM_ENABLE_POPS_2P=1

Build the selected RP2040-zero or Pico 2 W firmware. Both use the same P2
protocol definitions. For the RP2040-zero build:

	cd rp2040-zero
	mkdir -p build
	cd build
	cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
	make

Protocol
--------

The original RemoteJoy JoyEvent payload is kept at 20 bytes:

	magic, type, value

The original event types are unchanged:

	1  button down
	2  button up
	3  analog Y
	4  analog X

POPS 2P adds these types:

	6   player-2 button down
	7   player-2 button up
	8   player-2 analog Y
	9   player-2 analog X
	10  player-2 status

`TYPE_P2_STATUS` uses `value=1` when Slot 2 is present and `value=0` when Slot
2 is absent. The PSP plugin uses this as the switch for enabling or disabling
the POPS 2P patch.

RP2040-zero Side
----------------

The RP2040-zero firmware reads the first two slots from a PS1 multitap. Slot 1
is converted through the normal PS2-to-PSP mapping and emitted as the original
event stream.

Slot 2 is converted through the same mapping, but emitted as `TYPE_P2_*`
events. This keeps the normal input stream and the POPS player-2 stream
separate.

When Slot 2 changes between present and absent, the firmware sends
`TYPE_P2_STATUS`. On disconnect it also returns the cached Slot 2 state to
neutral so the PSP side releases any held player-2 buttons.

Pico 2 W Side
-------------

The Pico 2 W firmware assigns connected Bluetooth controllers to its
persistent 1P/2P slots by address. Both slots use the active shared Web mapping
profile. Slot 1 emits the original event types and Slot 2 emits `TYPE_P2_*`
events plus `TYPE_P2_STATUS` on connection changes.

Button transitions are queued from Bluepad32 reports immediately, while
analog values use the configured dead zones. Disconnecting Slot 2 sends a
neutral state and disables the POPS port-B hook through `TYPE_P2_STATUS`.

PSP Plugin Side
---------------

The PSP plugin maintains two input states:

	g_currjoy  normal remotejoy-minus input
	g_p2joy    POPS player-2 input

Normal JoyEvents update `g_currjoy`. Player-2 JoyEvents update `g_p2joy`.

The normal sceCtrl hook path continues to merge `g_currjoy` into PSP controller
reads, matching the single-controller behavior.

When POPS player-2 support is active, the plugin patches a small POPS gate
function so POPS will poll the second PS1 controller slot. The port-B
`sceCtrlPeekBufferNegative` path is then detected via the caller `$s0` captured
by `pops_hook.S`, and the returned buffer is filled from `g_p2joy`.

The port-B buffer is not synthesized from scratch. The plugin first calls the
real `sceCtrlPeekBufferNegative` implementation and keeps POPS's expected
hardware/system bits intact, then replaces only the game-relevant buttons and
analog values. This is important because POPS rejects or mishandles buffers
that do not look like real PSP controller reads.

L2/R2 Handling
--------------

PS1 L2/R2 in POPS are handled through POPS's own controller-assignment logic,
not by directly injecting private L2/R2 button bits.

For PS2 controller input, remotejoy-minus maps:

	PS2 L2      -> PSP analog left
	PS2 R2      -> PSP analog right
	PS2 L2+R2   -> PSP analog up

This same virtual-analog mapping is used in POPS for both the normal input
stream and the player-2 stream. For titles that need PS1 L2/R2, open the POPS
HOME menu and set the controller assignment so the PSP analog-pad directions
are mapped to the desired PS1 L2/R2 actions.

The plugin keeps POPS in charge of PS1 button assignment. This keeps the 2P
path consistent with POPS's normal controller settings and avoids carrying a
title-specific mapping inside remotejoy-minus.

POPS Loading
------------

With `RJM_ENABLE_POPS_2P=1`, the plugin reserves itself for POPS during the
VSH/GAME-side startup path using the CFW reboot-load mechanism when available.
On tested setups this allowed POPS support even when the PRX was not explicitly
listed in POPS.txt.

If a CFW does not carry the plugin into POPS through that path, enable the same
PRX from POPS.txt or the CFW-specific plugin configuration.

Known Hardware
--------------

Confirmed:

	SCPH-1070  PS1 multitap, Slot 1/Slot 2 detected

Not supported by the current wiring:

	SCPH-10090 PS2 multitap

SCPH-10090 did not respond to the controller-port-only bitbang wiring. It
likely requires PS2 multitap-specific behavior and/or memory-card-side signals,
so it is out of scope for the current RP2040-zero firmware.

Limitations
-----------

POPS 2P support relies on POPS internal code patterns and offsets observed on
the tested firmware/modules. The plugin verifies a small instruction signature
before patching, but this is still less stable than public PSPSDK APIs.

The POPS 2P hook is installed only when Slot 2 is present. If a title behaves
unexpectedly, retest with Slot 2 disconnected to confirm that stock POPS
behavior returns.

Use the POPS HOME-menu controller assignment with the PSP analog-pad mapping
described above for titles that need PS1 L2/R2.

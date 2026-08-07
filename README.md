remotejoy-minus standalone
==========================

This is the RP2040-zero standalone version of remotejoy, with the video output function removed and enabling control via PS1/PS2 controllers.  
By combining it with the external output function of the PSP-2000/3000, remote play using a TV screen is possible without a PC.  
Does not require psplink.prx or usbhostfs.prx on the PSP side.  

Build PSP Plugin
----------------

Set PSPDEV/PATH if needed, then run:

	make

The output is:

	remotejoy-minus.prx

Build with POPS 2P support:

	make clean
	make RJM_ENABLE_POPS_2P=1

POPS 2P support is intended for the RP2040-zero firmware's PS1 multitap input
path. See [POPS_2P.md](./POPS_2P.md) for details.

Diagnostic log build:

	make clean
	make RJM_ENABLE_LOG=1

The diagnostic build writes ms0:/rjm_standalone.log. The normal build does not
write a log file.

Build RP2040-zero Firmware
--------------------------

The direct PSP USB host firmware is in:

	rp2040-zero/

Build it with Pico SDK from this directory:

	cd rp2040-zero
	mkdir -p build
	cd build
	cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
	make

The output UF2 is:

	rp2040-zero/build/remotejoy_minus_standalone_usbhost.uf2

For wiring instructions between the RP2040-zero and a PS1/PS2 controller, please refer to [./rp2040-zero/README.md](./rp2040-zero/README.md).

POPS 2P Support
---------------

With a compatible PS1 multitap such as SCPH-1070, the RP2040-zero firmware can
read two controller slots:

	Slot 1  normal RemoteJoyMinus input
	Slot 2  POPS player-2 input

Slot 1 keeps the same behavior as the normal single-controller setup. In POPS,
it follows the PSP's own controller assignment behavior instead of being forced
to PS1 player 1.

Slot 2 is sent as a separate player-2 event stream. The PSP plugin enables the
POPS 2P hook only while Slot 2 is detected, so a single-controller setup keeps
the normal POPS behavior.

For POPS titles that need PS1 L2/R2, configure the POPS HOME-menu controller
assignment so the PSP analog pad directions are mapped to L2/R2. remotejoy-minus
intentionally carries PS2 L2/R2 through the PSP analog-pad path in POPS, because
that keeps POPS's own mapping logic in control.

This was tested with SCPH-1070. The PS2 multitap SCPH-10090 did not respond on
the controller-port-only bitbang wiring used by this project.

Install
-------

Write the firmware (.uf2) to RP2040-zero.

On the PSP side, copy remotejoy-minus.prx to the seplugins folder and add the following line to VSH.txt/GAME.txt/POPS.txt as appropriate.

	ms0:/seplugins/remotejoy-minus.prx

When built with `RJM_ENABLE_POPS_2P=1`, enabling the plugin from VSH/GAME is
enough for the plugin to reserve itself for POPS on compatible CFW setups. If
your CFW does not load it for POPS that way, also enable the same PRX from
POPS.txt or the CFW-specific plugin configuration.

Enable the plugin from the PSP recovery menu.  
Do not enable remotejoy.prx or RemoteJoyLite.prx because they conflict.  

Acknowledgments
---------------

This implementation is based on the PSPLINKUSB/RemoteJoy controller protocol
and USBHostFS-style endpoint layout. The PSP-side input injection logic is
derived from the original RemoteJoy approach and then reduced to input-only
operation.

[RemoteJoyLite by Kethen](https://github.com/Kethen/RemoteJoyLite) was used as a technical reference for the standalone
USB PRX structure, controller hook strategy, and POPS behavior.

The RP2040-zero firmware uses Pico SDK and TinyUSB as build/runtime dependencies.

License
-------

remotejoy-minus is distributed under the same BSD license as
PSPLINKUSB.  
This tree includes code, protocol definitions, and implementation structure derived from PSPLINKUSB RemoteJoy and usbhostfs, so the PSPLINKUSB BSD copyright notice, license conditions, and disclaimer must be preserved when
redistributing source or binaries.

In particular, remotejoy.h is copied from the original RemoteJoy source and retains its own license header.

The RP2040-zero firmware links against Pico SDK and TinyUSB. When distributing RP2040-zero source or UF2 binaries, also comply with the license terms of those projects.

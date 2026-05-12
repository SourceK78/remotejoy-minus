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

Install
-------

Write the firmware (.uf2) to RP2040-zero.

On the PSP side, copy remotejoy-minus.prx to the seplugins folder and add the following lines to VSH.txt/GAME.txt/POPS.txt as appropriate.

	ms0:/seplugins/remotejoy-minus.prx

Enable the plugin from the PSP recovery menu.  
Do not enable remotejoy.prx or RmoteJoyLite.prx because they conflict.  

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

remotejoy-minus is be distributed under the same BSD license as
PSPLINKUSB.  
This tree includes code, protocol definitions, and implementation structure derived from PSPLINKUSB RemoteJoy and usbhostfs, so the PSPLINKUSB BSD copyright notice, license conditions, and disclaimer must be preserved when
redistributing source or binaries.

In particular, remotejoy.h is copied from the original RemoteJoy source and retains its own license header.

The RP2040-zero firmware links against Pico SDK and TinyUSB. When distributing RP2040-zero source or UF2 binaries, also comply with the license terms of those projects.

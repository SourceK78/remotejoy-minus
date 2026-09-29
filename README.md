# remotejoy-minus standalone

This is the RP2040-zero/Raspberry Pi Pico 2W standalone version of remotejoy, with the video output function removed and enabling control via external controllers.
By combining it with the external output function of the PSP-2000/3000, remote play using a TV screen is possible without a PC.
Does not require psplink.prx or usbhostfs.prx on the PSP side.

The repository contains the PSP plugin and two input firmware options:

| Firmware | Input | Configuration | Players |
|---|---|---|---|
| [Pico 2 W](./pico2w/README.md) | Bluetooth via joypad-os | Wi-Fi Web portal | 1P and optional POPS 2P |
| [RP2040-Zero](./rp2040-zero/README.md) | Wired PS1/PS2 controllers | Compile-time | 1P and PS1 multitap POPS 2P |

The Pico 2 W firmware supports persistent controller pairing, shared mapping
profiles, configurable RGB indication, multilingual browser configuration,
and JSON mapping import/export. See its dedicated README for hardware and
setup instructions.

## PSP plugin build

Install PSPSDK/VitaSDK and ensure `psp-config` is available, then run:

```sh
make
```

The output is `remotejoy-minus.prx`.

To enable the separate player-2 stream for two-player POPS titles:

```sh
make clean
make RJM_ENABLE_POPS_2P=1
```

See [POPS_2P.md](./POPS_2P.md) for the POPS controller-assignment behavior and
known compatibility details.

To enable POPS GunCon emulation for gyro aiming from the Pico 2 W firmware
(see [POPS GunCon emulation](#pops-guncon-emulation)):

```sh
make clean
make RJM_ENABLE_POPS_AIM=1
```

It can be combined with `RJM_ENABLE_POPS_2P=1`. The on-screen crosshair assumes
POPS's "Original" screen mode, where the 320x240 PS1 picture sits at 80,16 on
the 480x272 LCD. For another placement, pass the picture rectangle:

```sh
make RJM_ENABLE_POPS_AIM=1 \
  RJM_POPS_AIM_SCREEN="RJM_POPS_AIM_SCREEN_X=112 RJM_POPS_AIM_SCREEN_W=256"
```

On TV out the picture is assumed to be scaled uniformly to fit the output and
centred. A measured TV rectangle can be given with `RJM_POPS_AIM_TV_X`,
`RJM_POPS_AIM_TV_Y`, `RJM_POPS_AIM_TV_W`, and `RJM_POPS_AIM_TV_H` in the same way.

For a diagnostic plugin that writes `ms0:/rjm_standalone.log`:

```sh
make clean
make RJM_ENABLE_LOG=1
```

Normal builds do not write a log file.

## PSP installation

1. Copy `remotejoy-minus.prx` to the PSP `seplugins` directory.
2. Add the following line to `VSH.txt`, `GAME.txt`, or `POPS.txt` as needed:

   ```text
   ms0:/seplugins/remotejoy-minus.prx
   ```

3. Enable the plugin from the CFW recovery/plugin menu.

Do not enable `remotejoy.prx` or `RemoteJoyLite.prx` at the same time; they use
the same PSP USB/controller facilities and conflict with this plugin.

When built with `RJM_ENABLE_POPS_2P=1`, loading the plugin from VSH/GAME is
normally sufficient for the plugin to reserve itself for POPS. Some CFW setups
may also require the entry in `POPS.txt`.

## Firmware builds

### Raspberry Pi Pico 2 W

The Bluetooth/Web-config firmware is in [`pico2w/`](./pico2w/README.md). Its
UF2 output is:

```text
pico2w/build/remotejoy_minus_pico2w.uf2
```

### RP2040-Zero

The wired PS1/PS2 firmware is in [`rp2040-zero/`](./rp2040-zero/README.md).
Its UF2 output is:

```text
rp2040-zero/build/remotejoy_minus_rp2040.uf2
```

## POPS 2P overview

The optional P2 protocol keeps the original RemoteJoy event types unchanged
and adds a separate player-2 button, analog, and connection-status stream.
Player 1 remains the normal remotejoy-minus input. Player 2 is injected through
the POPS port-B hook only while a second controller is connected.

POPS's HOME-menu controller assignment remains in control of the normal input.
For games requiring PS1 L2/R2, configure the POPS controller assignment as
described in [POPS_2P.md](./POPS_2P.md).

The wired firmware has been tested with the SCPH-1070 PS1 multitap. The
SCPH-10090 PS2 multitap did not respond to its controller-port-only bitbang
wiring.

## POPS GunCon emulation

With `RJM_ENABLE_POPS_AIM=1`, the plugin can present POPS controller port 1 to
the PS1 game as a Namco GunCon, so GunCon-compatible PS1 games can be played
with a gyro-equipped controller. No per-game patch is needed.

- The Pico 2 W firmware sends an absolute aim position while the active mapping
  profile has **GunCon mode** enabled (see
  [pico2w/README.md](./pico2w/README.md)). Firmware that sends no aim, such as
  the RP2040-Zero firmware, leaves port 1 as the normal digital pad.
- POPS emulates the PS1 controller protocol with a per-port state block in PSP
  scratchpad RAM. While aim data arrives, the plugin sets port 1's controller
  ID to the GunCon (`0x63`) and supplies the gun X/Y coordinates. When aiming
  stops, the digital pad (`0x41`) is restored. The block is only modified when
  its contents match the expected POPS layout.
- Off-screen aim is reported as the GunCon "no light" value (X=01h, Y=0Ah).
- Buttons come from the normal remotejoy-minus input. On the GunCon, the
  trigger is Circle, A is START, and B is Cross.
- GunCon games hide their own cursor, so the plugin draws a small crosshair on
  the PSP screen while aiming on screen.
- Only player 1 is converted. In POPS 2P, player 2 remains a digital pad.

Tested with Time Crisis and Biohazard Gun Survivor.

## Acknowledgments

This implementation uses the PSPLINKUSB/RemoteJoy controller protocol and a
USBHostFS-compatible endpoint layout. The PSP input-injection logic derives
from the original RemoteJoy approach and is reduced to input-only operation.

[RemoteJoyLite by Kethen](https://github.com/Kethen/RemoteJoyLite) was used as
a technical reference for the standalone USB PRX structure, controller hooks,
and POPS behavior.

The firmware uses Raspberry Pi Pico SDK and TinyUSB. The Pico 2 W build also
uses [joypad-os](https://github.com/joypad-ai/joypad-os) and BTstack.

## License

Unless otherwise noted, `remotejoy-minus` is distributed under the same BSD
license as PSPLINKUSB. Third-party files retain their respective licenses; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
This tree contains code and protocol structure derived from PSPLINKUSB
RemoteJoy/usbhostfs, so its copyright notice, license conditions, and
disclaimer must be preserved when redistributing source or binaries.

`remotejoy.h` retains its original license header. When distributing firmware,
also comply with the licenses of Pico SDK, TinyUSB, joypad-os, BTstack, and the
vendored pico-examples/MicroPython components. Binary distributions should
include the license and notice files identified in `THIRD_PARTY_NOTICES.md`.

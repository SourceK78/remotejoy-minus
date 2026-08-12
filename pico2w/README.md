# remotejoy-minus for Raspberry Pi Pico 2 W

This firmware connects two Bluetooth controllers through Bluepad32, applies a
shared Web-configured mapping profile, and sends remotejoy-minus input events to
a PSP over USB. It is protocol-compatible with
[`rp2040-zero/remotejoy_minus_protocol.h`](../rp2040-zero/remotejoy_minus_protocol.h).

## Features

- Two persistent Bluetooth controller slots for player 1 and player 2.
- DualSense, DualShock 4, supported Xbox Wireless controllers, DualShock 3,
  and other controllers supported by Bluepad32.
- Up to eight mapping profiles shared by both players.
- Controller-centric mapping: each controller input selects a PSP button or
  the internal `COMBO` action.
- Fixed left-stick to PSP analog-pad mapping.
- Independently mappable right-stick up/right/down/left directions.
- Per-profile left/right stick dead zones and RGB color.
- `START` + an input mapped to `COMBO` cycles the active profile.
- Persistent mappings and active-profile selection.
- Browser JSON import/export without Bluetooth addresses or private bond keys.
- Wi-Fi configuration portal with eight UI languages.
- remotejoy-minus USB host handshake and 1P/2P event delivery.

## Required hardware

- Raspberry Pi Pico 2 W.
- Two single-color player-status LEDs and suitable series resistors.
- One common-cathode RGB LED and three suitable series resistors, or equivalent
  active-high RGB channels.
- A USB connection from the Pico 2 W host to the PSP device.
- A regulated 5 V supply connected to the Pico 2 W `VBUS` pin/USB VBUS rail.
  This rail must also provide host VBUS to the PSP device side.

### GPIO assignment

| GPIO | Function |
|---|---|
| 18 | Player 1 status LED (PWM) |
| 19 | Player 2 status LED (PWM) |
| 20 | RGB red (PWM) |
| 21 | RGB green (PWM) |
| 22 | RGB blue (PWM) |

The current code assumes active-high LED channels. Adapt `runtime_ui.c` if the
hardware uses a common-anode RGB LED or active-low drivers.

## USB power and topology

The Pico is the USB **host** and the PSP is the USB **device**. A normal hub
only works when the Pico is connected to the hub's upstream/host side and the
PSP is connected to a downstream port. Connecting both devices to downstream
ports only supplies power and does not create a USB data path.

The tested wiring requires regulated 5 V on the Pico 2 W **VBUS** pin/USB VBUS
rail. Powering the Pico only through **VSYS is not sufficient**: the Pico itself
can run, but VSYS does not drive USB VBUS back toward the connector, so the PSP
does not detect a powered USB host and the USB connection is not established.

## PSP setup

Build and install the repository's `remotejoy-minus.prx` as described in the
[top-level README](../README.md). Do not load RemoteJoyLite or another
RemoteJoy plugin at the same time.

For POPS player 2, build the PSP plugin with:

```sh
make clean
make RJM_ENABLE_POPS_2P=1
```

## First-time controller setup

1. Flash `remotejoy_minus_pico2w_config.uf2` to the Pico 2 W.
2. Power the Pico normally. The configuration AP is disabled at startup.
3. Hold BOOTSEL for about 1.5 seconds.
4. Connect a PC or phone to:

   ```text
   SSID: RemoteJoy-Config
   Password: remotejoy
   ```

5. Open `http://192.168.4.1/`.
6. Select the empty 1P or 2P slot and put the controller in pairing mode.
7. Configure mappings and press **Finish setup** (or its translated label).

Finishing setup saves the mapping and reboots into normal mode. A full reboot
is intentional: it cleanly resets the shared CYW43 Wi-Fi/Bluetooth state and
makes later configuration-mode entry reliable. Pairing slots, Bluepad32 bond
keys, mappings, and the active profile survive reboot.

PSP output is released and disabled while the configuration portal is active.

## Web configuration

The portal provides:

- Pair, unpair, and status display for the 1P/2P slots.
- A persistent **Enable 2P** option. Disabling it keeps the 2P pairing data but
  excludes 2P from reconnection scanning and PSP input even in POPS.
- Mapping profile add, delete, select, and save.
- PSP-button assignment for controller buttons and right-stick directions.
- Left/right stick dead-zone sliders.
- Eight RGB presets plus custom R/G/B sliders; custom black means LED off.
- Mapping JSON export/import.
- The Pico Bluetooth address needed for DualShock 3 setup.
- Editable setup-mode WPA2 password. The SSID remains `RemoteJoy-Config`.
  Changes apply the next time setup mode starts. The password must contain
  8–63 printable ASCII characters and is never returned to the browser.

If the custom setup Wi-Fi credentials are lost, hold BOOTSEL for about six
seconds. Setup mode starts after the normal hold interval, then the Wi-Fi
credentials alone are reset to `RemoteJoy-Config` / `remotejoy` and the Pico
reboots. Controller slots and mapping profiles are preserved.

Supported UI languages are Japanese, English, Simplified Chinese, Traditional
Chinese, Korean, Spanish, French, and German. The browser language is used on
first access, and manual selection is saved in browser local storage.
Controller input names and PSP button names remain in English in every UI
language.

Mappings are shared by 1P and 2P. The left stick always controls the PSP analog
pad and therefore is not shown as a configurable mapping row. `COMBO` is an
internal action and is never sent as a PSP button. Holding controller `START`
with a mapped `COMBO` input advances to the next profile; `START` is suppressed
during that switching chord.

Bluetooth bond keys and controller slot identities are stored independently
from mapping profiles. Exported JSON contains mapping data and the 2P-enabled
setting, but not Bluetooth bond keys or controller identities.

The PSP plugin reports whether it is running in POPS through an optional,
backwards-compatible HostFS extension. In XMB and PSP games, only player 1 is
included in reconnection discovery and player-2 input is suppressed. An
already-connected player-2 controller is left connected, but its later
disconnection does not restart discovery. In POPS, player 2 is included when
**Enable 2P** is selected. Discovery stops once every currently applicable,
registered player is connected, and always resumes for Web pairing.

## LED behavior

### Player LEDs (GPIO18/19)

| Slot state | Indication |
|---|---|
| Empty | Off |
| Pairing search | Fast blink at full brightness |
| Registered, waiting to reconnect | Steady at about 20% brightness |
| Connected | Steady at full brightness |

These indications remain active in both normal and configuration modes.

### RGB LED (GPIO20/21/22)

| System state | Indication |
|---|---|
| Normal mode, PSP USB not connected | Solid red |
| Normal mode, PSP USB connected | Active mapping-profile color |
| Configuration mode | Slow red breathing effect |

Selecting custom black for a profile turns the RGB LED off after the PSP USB
connection is established.

## Controller notes

### DualSense and DualShock 4

Use their normal Bluetooth pairing combinations. After a Pico soft reboot they
may require the PS button to reconnect.

### DualShock 3

DualShock 3 requires manual pairing. Use the Bluetooth address shown in the
portal and write it to the controller over USB with Bluepad32's
`sixaxispairer` tool. Then select a Pico slot and press the DS3 PS button.
Connection may take more than ten seconds. See the
[Bluepad32 DS3 guide](https://bluepad32.readthedocs.io/en/latest/pair_ds3/).

Pair non-DS3 controllers with **DualShock 3 mode** disabled first. Enable the
mode when pairing or reconnecting a DS3; disabling it prevents a DS3 from
reconnecting. When a DS3 and DualSense are connected together in this mode,
the firmware automatically uses a 20 ms Bluetooth Sniff interval for the
DualSense link to balance airtime between the two controllers. Other tested
combinations, including DualSense with 8BitDo and DS3 with 8BitDo, are not
throttled.

### 8BitDo controllers

Many 8BitDo controllers support multiple startup and D-pad reporting modes.
Depending on the model and saved mode, a physical D-pad may be reported as the
left or right analog stick instead of as D-pad buttons. In this firmware that
means it controls the fixed PSP analog-pad path rather than the configurable
`D-pad Up/Right/Down/Left` inputs.

Use the model's documented shortcut or mode switch to select normal D-pad
reporting. For example, on the M30 Bluetooth controller, hold
`SELECT + D-pad Up` for about five seconds; its LED blinks red when the reset
succeeds. Other 8BitDo models use different button combinations, so consult
the corresponding 8BitDo manual rather than assuming the M30 shortcut applies.

## Input and USB behavior

Bluepad32 reports are normalized, mapped, and assigned to a player by the
persisted Bluetooth address. Button and D-pad transitions are queued
immediately so short fighting-game inputs are not lost. Analog values are
updated every 10 ms with the configured dead zones.

TinyUSB runs in host mode, discovers the PSP vendor interface (`ff/01/ff`),
opens the three USBHostFS-compatible bulk endpoints, completes the HostFS hello
handshake, and sends the original remotejoy-minus event layout plus the optional
P2 event types.

The native USB port is reserved for PSP host operation, so USB serial logging
is disabled in this build.

## Build

Requirements:

- Pico SDK 2.1 or newer with its submodules initialized.
- Bluepad32 submodule initialized at `pico2w/external/bluepad32`.
- CMake, GNU Make or Ninja, and the Arm GNU Toolchain (`arm-none-eabi-gcc`).

Example Linux build:

```sh
git submodule update --init --recursive
sh pico2w/scripts/setup-bluepad32.sh

export PICO_SDK_PATH="$HOME/src/pico-sdk"
git -C "$PICO_SDK_PATH" submodule update --init --recursive

cmake -S pico2w -B pico2w/build \
  -DPICO_SDK_PATH="$PICO_SDK_PATH" \
  -DPICO_BOARD=pico2_w

cmake --build pico2w/build --parallel "$(nproc)"
```

The setup script applies the runtime compatibility patch stored in
`pico2w/patches/bluepad32-runtime.patch`. It is safe to run more than once and
stops if the submodule revision or local changes conflict with the patch. The
patch contains only the DS3/normal-mode security handling and the DualSense
duplicate-callback fix required by this firmware. Bluepad32's
`sixaxispairer` remains unmodified.

For Windows development, the equivalent patch helper remains available as
`pico2w/scripts/setup-bluepad32.ps1`.

Output:

```text
pico2w/build/remotejoy_minus_pico2w_config.uf2
```

## Troubleshooting

- **The portal does not open on Android:** disable mobile-data fallback for
  the `RemoteJoy-Config` network and open `http://192.168.4.1/` explicitly.
- **A controller does not reconnect after reboot:** press its Home/PS button.
- **The RGB LED remains red:** the PSP USB HostFS handshake is incomplete;
  check the plugin, USB topology, D+/D-, ground, and that regulated 5 V is
  connected to Pico VBUS—not only VSYS.
- **Both Pico and PSP are connected to ordinary hub ports:** both are on the
  downstream side; connect the Pico to the upstream/host side instead.
- **An 8BitDo D-pad controls the analog pad:** switch that controller to normal
  D-pad reporting using its model-specific shortcut. For M30 Bluetooth, hold
  `SELECT + D-pad Up` for five seconds.
- **DS3 does not appear in a normal scan:** program the Pico Bluetooth address
  into the DS3 first; normal Bluetooth pairing is not implemented by DS3.

## Dependencies and licenses

This firmware uses Raspberry Pi Pico SDK, TinyUSB, Bluepad32, and BTstack.
The captive-portal DHCP/DNS sources are vendored from MicroPython and
Raspberry Pi pico-examples with their original MIT/BSD notices and upstream
revision recorded under `pico2w/third_party/pico-examples`.

Follow the corresponding license terms when redistributing source or UF2
binaries. Bluepad32 is Apache-2.0; BTstack licensing conditions also apply.
See the repository-level `THIRD_PARTY_NOTICES.md` for attribution and binary
distribution guidance.

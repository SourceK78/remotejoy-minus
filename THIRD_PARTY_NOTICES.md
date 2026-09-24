# Third-party notices

Unless otherwise noted, remotejoy-minus is licensed under the BSD 3-Clause
License in `LICENSE`. Third-party components retain their own licenses.

## joypad-os

joypad-os is licensed under the Apache License 2.0. It is included as the
`pico2w/external/joypad-os` Git submodule. Its license text is available at
`pico2w/external/joypad-os/LICENSE` after submodule initialization.

The patches under `pico2w/patches` adapt joypad-os for this firmware's Classic
Bluetooth, DualShock 3, controller-forgetting, and 8BitDo reconnection behavior.
These modifications do not change joypad-os's Apache License 2.0 terms.

The Pico SDK provides BTstack for the Pico 2 W build. Review the applicable
BTstack/BlueKitchen license terms before commercial distribution or product
integration.

## Raspberry Pi pico-examples

The captive-portal DNS server under
`pico2w/third_party/pico-examples/access_point` is vendored from Raspberry Pi's
pico-examples repository under the BSD 3-Clause License. See
`pico2w/third_party/pico-examples/LICENSE.pico-examples.txt` and `UPSTREAM.md`.

## MicroPython DHCP server

The DHCP server files in the same directory originate from MicroPython and are
licensed under the MIT License. Their source headers are retained; see
`pico2w/third_party/pico-examples/LICENSE.micropython.txt`.

## Binary distribution

When redistributing UF2 or other binaries, include the repository `LICENSE`,
this notice, the joypad-os Apache License 2.0 text, and the vendored
pico-examples/MicroPython license texts in the accompanying materials.

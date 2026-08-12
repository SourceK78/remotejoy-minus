# Third-party notices

Unless otherwise noted, remotejoy-minus is licensed under the BSD 3-Clause
License in `LICENSE`. Third-party components retain their own licenses.

## Bluepad32

Bluepad32 is Copyright 2019 Ricardo Quesada and contributors and is licensed
under the Apache License 2.0. It is included as the
`pico2w/external/bluepad32` Git submodule. Its license text is available at
`pico2w/external/bluepad32/LICENSE` after submodule initialization.

`pico2w/patches/bluepad32-runtime.patch` modifies Bluepad32 for runtime
Bluetooth security selection, DS3 compatibility, 8BitDo reconnection handling,
and DualSense callback handling. These modifications do not change
Bluepad32's Apache License 2.0 terms.

Bluepad32 uses BTstack. The Bluepad32 license notice states that BTstack is
free for open-source projects but may require a BlueKitchen commercial license
for commercial use. Review the current BTstack/BlueKitchen terms before
commercial distribution or product integration.

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
this notice, the Bluepad32 Apache License 2.0 text, and the vendored
pico-examples/MicroPython license texts in the accompanying materials.

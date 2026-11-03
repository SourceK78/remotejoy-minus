# TinyUSB patch

`ncm_device.c` is the Pico SDK 2.2.0 TinyUSB 0.18.0 copy
(`pico-sdk/lib/tinyusb/src/class/net/ncm_device.c`) with one change:

- `wNdbInDivisor` / `wNdbOutDivisor` 1 -> 4 (upstream
  [PR #3914](https://github.com/hathach/tinyusb/pull/3914)). Since the
  September 2026 Windows 11 update KB5124008, usbncm.sys rejects 1 and the Web
  configuration mode fails with Code 10.

CMakeLists.txt builds this file instead of the SDK's. Remove it once the Pico
SDK ships a TinyUSB release with the fix.

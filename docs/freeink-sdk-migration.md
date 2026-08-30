# FreeInk SDK migration

The X3/X4 firmware is built against the pinned `freeink-sdk` submodule rather
than the retired `open-x4-sdk` fork.

## Version and scope

- Firmware build version: `1.5.0`
- SDK commit: `3e4d65159605f2a53d5435824f9c24e62ea80d9e`
- Upstream migration reference: `ebebc6f2`
- Supported profiles: standard X3 and X4, selected at runtime
- Deferred profiles: X4 Pro touch/frontlight/USB MSC/PSRAM

The SDK switch does not change EPUB, settings, bookmarks, reading progress or
image/font cache formats. The current branch's EPUB section version (41) and
CSS cache version (10) remain the source of truth; incompatible caches are
rebuilt by their normal version checks. A suspended section may use the
version-41 partial sentinel (`0xF1`) and is extended lazily on the next open.
Section 40-or-earlier (including older partial sentinels) and CSS 9-or-earlier
files are discarded and rebuilt, while user settings,
bookmarks and reading progress are kept.

## HAL boundary

Non-UI application code must use `HalDisplay`, `HalGPIO`, `HalPowerManager`,
`HalStorage`, `HalClock`, and `HalTiltSensor`. `HalSdkCompat.h` is the
compatibility boundary for SDK type aliases and refresh-mode conversion. FUI
screens include FreeInkUI only through `UiAppHost`/`UiListActivity`; readers
and other legacy activities continue to use the HAL and renderer interfaces.

The display wrapper preserves X3 grayscale preconditioning, plane streaming,
refresh modes and the legacy `displayWindow()` call. Since windowed refresh is
not part of the current public FreeInk display API, that call safely falls back
to a regular refresh.

## Runtime detection and sleep

`HalGPIO` first honors the existing NVS override/cache, then uses FreeInk's
Xteink detector and finally the legacy I2C fingerprint probe. The selected
profile is passed to `BoardConfig` before SPI ownership is established.

`HalPowerManager` keeps the existing CPU-frequency locks and battery polling,
while FreeInk owns board-specific sleep-rail handling. A standard X3/X4 build
must not define X4 Pro USB, touch or frontlight capability flags.

The RTC and IMU wrappers use FreeInk `Rtc` and `Imu` services; the existing
clock formatting and tilt gesture state machines remain in the firmware HAL.

## Network transport

`SecureHttpClient`/wolfSSL is used by KOReader sync and streamed downloads.
WeRead now uses the official SDK `SecureNet` wolfSSL path (including
`setInsecure()` and the `weread.qq.com` SNI fragment-size workaround).  The
ESP-IDF mbedTLS implementation remains only as a compatibility fallback for
legacy callers; WebSocket/PubSub protocols are unchanged.

## Updating the SDK

Do not float the submodule to `main` during a firmware release. Update the
gitlink and this document together, then repeat the X3/X4 build and device
regression matrix. If a rollback is required, restore the previous firmware
binary; no user-data migration is needed.

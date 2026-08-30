# FreeInkUI migration (X3/X4)

The `port/base-1.5-stability-x4` branch hosts the FreeInkUI migration on top of
`freeink-sdk` commit `3e4d6515`.  The application keeps the existing
`ActivityManager`, renderer, EPUB/TXT/XTC layout engine, and X4 extensions; FUI
owns the interaction table and the chrome for migrated screens.

## Screen mapping

`UiListActivity` is used by the file browser, recent books, OPDS browsing and
server lists, Wi-Fi mode/network lists, extension lists, font and language
selection, KOReader settings, status-bar settings, and EPUB/XTC chapter,
bookmark, footnote, and reader-menu screens.  AirPage settings and
history, front-button remapping, the home menu, WeRead chapter-range and
manage/detail action lists, and the book/day/achievement lists in reading
statistics use the same FUI list interaction and scrolling contract.
`SettingsActivity`
uses `UiTabListActivity` so the four settings categories share the same tab
ring and per-tab viewport behavior.  `UiScreenActivity` and `UiAppHost` are
used by stateful pages (network prompts, KOReader sync choices, UTC offset,
interval and percent sliders, timers, QR/image and statistics charts) while
their existing models and long-running operations stay in the activity layer.
Home cover tiles, WeRead's shelf grid and multi-stage network pages, and
specialised reader/image canvases remain renderer-owned where their variable
layout cannot be represented by a single FUI list.

Migrated data/settings rows explicitly leave `ListItem::icon` empty.  The
home, extension, and flashcard menus keep theme-aware icon slots for themes
that use them; `RoundedRaffExtTheme` returns `false` from
`showsFuiMenuIcon()`, so those menus remain text-first under RoundedRaffExt.

Touch events are converted by `MappedInputManager` into a FreeInkUI snapshot;
button mappings, orientation-aware navigation, hold/release semantics, and
legacy row hit tests remain available for pages that still use the renderer
directly.  The interaction table is published only after a complete render,
so a tap cannot observe a half-built list.  `UiListActivity::onExit()` closes
the published route before row buffers are released; list pages with custom
cleanup call that base hook first.

List selection and viewport updates are guarded by the activity render lock;
wrapped CJK labels use the measured page size and a bounded rebuild pass.  The
FUI theme bridge also forwards list, header, control, sheet, and slider-capsule
metrics from Classic, Lyra, and RoundedRaff instead of relying on SDK defaults.

## Font coverage

The UI font family uses Ubuntu Medium-style regular/bold glyphs at 10px and
12px, with Noto Sans SC and Noto Sans JP fallbacks.  Built-in UI glyphs include
ASCII/Latin plus the existing Simplified Chinese list and these Japanese
ranges: U+3000–U+303F, U+3040–U+309F, U+30A0–U+30FF, U+31F0–U+31FF, and
U+FF00–U+FFEF.  `ui_ja_chars.txt` supplies additional common Japanese UI
kanji.  The 8px status/small font is generated with the same coverage.

Book text is not artificially limited by the UI subset: characters absent from
the built-in family continue through the existing SD-card font fallback.
Arabic, Hebrew, Vietnamese, and other extra UI font packs are intentionally
not part of this target.

To regenerate fonts on a development machine, set `NOTOSANS_SC_FONT` and
`NOTOSANS_JP_FONT` to local font files and run
`lib/EpdFont/scripts/convert-builtin-fonts.sh`, then regenerate `src/fontIds.h`.
The converter disables optional kerning when a fallback creates more than 255
kerning classes; this avoids uint8 overflow and preserves deterministic glyph
advances on the device.

## Hardware boundary

The build targets standard X3/X4 (`FREEINK_DEVICE_X3` and
`FREEINK_DEVICE_X4`).  It does not enable X4 Pro touch/front-light, USB MSC,
PSRAM, or `BitmapFont`/`DisplayTarget`.  Display, power, SD, network/TLS, and
board detection continue through the existing HAL and the locked
`freeink-sdk` compatibility surface.  X3 fingerprint detection and the X4
button map remain fallback paths when a board profile is unavailable.

## Caches and rollback

FUI changes do not alter the current EPUB section (41), CSS cache (10), image,
bookmark, or reading-state formats. A version-41 partial section cache stores
the readable page prefix and parse watermark and is extended on the next open.
Existing section 40-or-earlier (including older partial sentinels) and CSS
9-or-earlier files are rejected by their normal version checks and rebuilt;
user settings, bookmarks, and reading progress are retained.  To roll back this
UI migration, restore the previous application commit while leaving the
`freeink-sdk` submodule and user data in place; no FUI-specific persistent files
are created.  If a later SDK update adds its own cache header, it must use a
separate SDK cache version rather than the EPUB section version.

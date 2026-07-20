# Session Handoff — Calloway OS / IOT_Dashboard

Date: 2026-07-18
Commit: `dc8e206` — pushed to `origin/main` on [github.com/LoganCalloway/calloway-os](https://github.com/LoganCalloway/calloway-os)

## What this session was

A readability pass on `src/main.cpp` (the single-file ESP32-C6 firmware), plus one small feature add. No hardware changes, no new sensors — pure code organization, a couple of real bugs found and fixed along the way, and one new UI element.

## What changed

**Extracted the web dashboard page.** `PAGE_HTML` (the omnicore.local dashboard's HTML/CSS/JS, ~400 lines) moved out of `main.cpp` into `include/webpage.h`. It's still `#include`d back into `main.cpp` — same single compiled file, just easier to find and edit the web page without scrolling past firmware logic.

**Grouped related globals into structs:**
- `SensorReadings` — `lux`, `co2`, `pm1/25/4/10`, `voc`, plus `scd40Valid`/`sen54Valid` living next to the fields they gate (previously scattered across two separate blocks).
- `TouchState` — touch position/timing bookkeeping (was six separate globals).
- `Settings` vs `UiState` — split what actually gets persisted to NVS (`isNightMode`, `isCelsius`, `isMilitaryTime`, `isAutoDim`, `brightness`) from what resets every boot (`menuOpen`, `menuOffset`, etc.). Makes `loadSettings()`/`saveSettings()` obviously complete at a glance.

**Named magic numbers that were duplicated between drawing and touch hit-testing** — the menu close button, scroll arrows, and reset-confirm YES/NO buttons all now share one constant between the code that draws them and the code that hit-tests them, so they can't silently drift out of sync if one gets tweaked and the other doesn't. Also gave the WiFi setup AP name (`"OmniCore-Setup"`) a single named constant instead of two hardcoded copies.

**Fixed `drawWiFiIcon()` taking `x, y` parameters that never varied** — both call sites always passed the same constants. Dropped the parameters; it reads `WIFI_ICON_X`/`WIFI_ICON_Y` internally now, matching every other display function.

**Extended Night Mode to the whole UI.** The settings menu, brightness slider, and reset-confirm dialog previously hardcoded day-mode `TFT_*` colors directly, so toggling Night Mode only dimmed the dashboard — the menu you'd actually be looking at to flip the toggle stayed full brightness. `ColorPalette` grew from 18 to 27 fields to cover this; switched its initializers from positional to C++20 designated (`.field = value`) since positional was getting risky at that size. Also named the recurring red-shift shades (`NIGHT_RED_BRIGHT/MED/DIM`, `NIGHT_AMBER_WARN`) instead of repeating raw hex across a dozen fields.

**Fixed a real bug**: the "NEURAL LINK ESTABLISHED" success message (shown once WiFi connects) was silently rendering in the tiny hint-text font instead of matching "ESTABLISHING NEURAL LINK" above it. A `setFont(NULL)` call for the small "Hold screen 3s to reset WiFi" hint was never reset back before the success message printed.

**Added an overall air-quality status indicator** to the physical TFT dashboard — a colored dot + "AIR QUALITY: GOOD/ELEVATED/POOR" label, sitting in the open strip between the clock's divider line and the PM row. Reuses the exact same worst-of-CO2-and-PM2.5 severity logic the web dashboard already computes for its own status banner. Position was nudged right once per your feedback (`STATUS_DOT_X`/`STATUS_TEXT_X` in the layout constants) — worth a final look once you've flashed it to confirm it lands where you want.

**Comment cleanup pass** — removed a few comments left stale by the refactor (referencing old `g_`-prefixed variable names that no longer exist) and reworded a couple that narrated past bugs ("previously X, now Y") instead of explaining the current code's reasoning, which is the more durable style.

## Known issue — don't re-split main.cpp into multiple files

We tried splitting the firmware into multiple `.cpp` files (`globals.cpp`, `display.cpp`, `network.cpp`, etc.) for readability and hit a wall: this project's specific PlatformIO/pioarduino toolchain build (`platform-espressif32` "stable" release, ESP32C6, arduino-esp32 3.3.7) fails intermittently and non-deterministically whenever more than one `.cpp` file `#include`s `<WiFi.h>` — a `'network_event_handle_t' does not name a type'` cascade inside the ESP32 core's own `WiFiGeneric.h`, hitting a different file on almost every rebuild. Spent a full session ruling out every code-level cause (header order, redundant includes, WiFiManager version pinning, disk space, orphaned build processes, sdkconfig variants) — the *single-file* version builds successfully every time, clean or dirty, no exceptions. The multi-file split doesn't, even in a fully isolated `rm -rf .pio` rebuild. Conclusion: it's an environment/toolchain instability, not fixable from source. **Stick with the single `main.cpp` unless you want to burn another session on it after a fresh PlatformIO install or a pioarduino platform update.** Safe alternative for organization: pulling large self-contained content (like `webpage.h`) into a header that's `#include`d back into `main.cpp` — that doesn't add a translation unit, so it doesn't trigger this.

## Not done / possible follow-ups

- **`Ticker` struct** for the six timing-interval/`lastXTime` pairs in `loop()` — proposed, you declined it specifically (prefer the explicit repeated pattern over the abstraction since it's more obvious at a glance). Not done, not planned unless you change your mind.
- **Weather globals** (`g_currentTemp`, `g_tempHigh`, `g_tempLow`, `g_skyStatus`) are still flat globals, not grouped into a `WeatherData` struct — flagged as a candidate early on but we prioritized sensor readings/touch state/settings instead and never circled back.
- **Making CO2/PM2.5 render larger/bolder** than the other PM/AQ row values, to match how they're already treated specially by color — raised as an idea alongside the status indicator, not implemented.
- **Verify the status indicator on real hardware** — only checked via a rough SVG mockup, not the actual ST7796 display.

## Build notes

`platformio` isn't on this Mac's shell `PATH` — use the full path:
```
/Users/logancalloway/.platformio/penv/bin/platformio run
```

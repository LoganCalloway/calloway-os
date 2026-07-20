# Calloway OS — Omni-Core Telemetry Station

A self-configuring desktop IoT dashboard built on the XIAO ESP32-C6. Pulls time from NTP, weather from OpenWeatherMap, and reads live environmental data from onboard sensors — with a full touch-driven UI on the physical display and a live web dashboard on the same network.

## Features

- **Time & weather** — NTP-synchronized clock, live weather from OpenWeatherMap, automatic location/timezone detection via IP geolocation (no manual configuration after first setup)
- **Environmental sensing** — TSL2591 (ambient light), SCD40 (CO2, temperature, humidity), SEN54 (PM1/PM2.5/PM4/PM10 particulate matter + VOC index)
- **Air quality status** — a single GOOD / ELEVATED / POOR verdict driven by CO2 and PM2.5 thresholds, shown on both the display and the web dashboard
- **Touch UI** — full capacitive touch settings menu (Night Mode, °F/°C, 12/24hr time, brightness, Auto Dim, WiFi reset) via FT6236, with settings persisted across reboots
- **Web dashboard** — served directly from the device at `omnicore.local`, with live sensor cards and 24-hour history charts (1/6/12/24-hour range toggle)
- **Reliability-first design** — hardware watchdog, HTTP timeouts, WiFi auto-reconnect, 6-hour restart fallback, and automatic DST resync — built to run unattended for months

## Hardware

| | |
|---|---|
| MCU | Seeed Studio XIAO ESP32-C6 |
| Display | ST7796 3.5" IPS TFT, 480×320, SPI |
| Touch | FT6236 capacitive touch controller |
| Sensors | TSL2591, SCD40, SEN54 |

## Building

Built with [PlatformIO](https://platformio.org/):

```bash
pio run                # build
pio run --target upload  # flash
```

Create `include/secrets.h` (gitignored, not tracked) with your OpenWeatherMap API key before first build:

```cpp
#define WEATHER_API_KEY "your-openweathermap-api-key"
```

WiFi credentials aren't hardcoded — the device runs a WiFiManager captive portal on first boot for network setup.

## Project Report

A full project report — hardware design, PCB and enclosure details, software architecture, and development history — is available at [docs/Omni-Core_Report.pdf](docs/Omni-Core_Report.pdf).

## Acknowledgments

- PCB designed in EasyEDA Standard, fabricated and assembled by JLCPCB
- Enclosure modeled in SolidWorks, 3D printed
- Firmware development assisted by Claude Code

## License

Copyright © 2025–2026 Logan Calloway. All rights reserved. See individual source file headers for details.

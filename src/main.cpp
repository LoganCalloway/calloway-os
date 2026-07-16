// =============================================================================
//  ██████╗ █████╗ ██╗     ██╗      ██████╗ ██╗    ██╗ █████╗ ██╗   ██╗
// ██╔════╝██╔══██╗██║     ██║     ██╔═══██╗██║    ██║██╔══██╗╚██╗ ██╔╝
// ██║     ███████║██║     ██║     ██║   ██║██║ █╗ ██║███████║ ╚████╔╝ 
// ██║     ██╔══██║██║     ██║     ██║   ██║██║███╗██║██╔══██║  ╚██╔╝  
// ╚██████╗██║  ██║███████╗███████╗╚██████╔╝╚███╔███╔╝██║  ██║   ██║   
//  ╚═════╝╚═╝  ╚═╝╚══════╝╚══════╝ ╚═════╝  ╚══╝╚══╝ ╚═╝  ╚═╝   ╚═╝   
// =============================================================================
//  Project      : Calloway OS
//  Product      : Omni-Core Telemetry Station
//  Platform     : XIAO ESP32-C6
//  Display      : ST7796 3.5" IPS TFT (480x320) with FT6236 capacitive touch
//  Version      : v2.0
//  Author       : Logan Calloway
//  License      : Copyright (c) 2025 Logan Calloway. All Rights Reserved.
//                 Unauthorized copying, distribution, or modification of
//                 this file, via any medium, is strictly prohibited.
// -----------------------------------------------------------------------------
//  Description  :
//    A desktop engineering dashboard built on the XIAO ESP32-C6. Pulls time
//    from NTP, weather from OpenWeatherMap, and reads live environmental data
//    from the TSL2591 (light), SCD40 (CO2, temp, humidity), and SEN54
//    (PM1/PM2.5/PM4/PM10 + VOC index). Full capacitive touch menu via FT6236. Settings
//    persist across reboots via NVS. Backlight driven by direct PWM to the
//    display LED pin. Auto Dim maps TSL2591 lux to brightness automatically.
// -----------------------------------------------------------------------------
//  Touch interaction :
//    Tap dashboard     — opens settings menu
//    Tap menu item     — toggles or acts immediately
//    Tap ^ / v         — scrolls menu up or down
//    Tap X button      — closes menu
//    Brightness item   — opens slider, drag to adjust live
//    Reset WiFi item   — shows YES/NO confirmation screen
//    Hold on connect   — hold screen 3s to reset WiFi during connection
// -----------------------------------------------------------------------------
//  Architecture :
//    DATA LAYER    — functions that fetch or calculate data, never draw
//    DISPLAY LAYER — functions that draw to the screen, never fetch
//    COORDINATOR   — loop() decides when everything runs, gates WiFi calls
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_FT6206.h>
#include <Adafruit_TSL2591.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2CSen5x.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

#include "secrets.h"

// =============================================================================
// COLOR DEFINITIONS
// =============================================================================
#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_ORANGE      0xFD20
#define TFT_DARKGREY    0x7BEF
#define TFT_LIGHTGREY   0xC618
#define TFT_MAROON      0x7800

// =============================================================================
// CONFIGURATION
// =============================================================================
#define OS_VERSION    "CALLOWAY_OS v2.0"
const char* weatherKey  = WEATHER_API_KEY;
String currentCity      = "Asheville,US";   // updated by syncLocationAndTime()
long   currentUtcOffset = -14400;           // updated by syncLocationAndTime()

// =============================================================================
// PIN MAPPING
// =============================================================================
#define TFT_RST   D0
#define TFT_CS    D1
#define TFT_PWM   D2   // backlight PWM — direct drive to display LED pin
#define TFT_DC    D3
#define TFT_SDA   D4   // I2C data — shared by all sensors and touch
#define TFT_SCL   D5   // I2C clock — shared by all sensors and touch
#define CTP_RST   D7   // touch controller hardware reset
#define TFT_SCK   D8   // SPI clock
#define CTP_INT   D9   // touch interrupt pin (wired, available for future use)
#define TFT_MOSI  D10  // SPI data

// =============================================================================
// LAYOUT CONSTANTS
// =============================================================================
#define SCREEN_W        480
#define SCREEN_H        320

#define WEATHER_X       290
#define WEATHER_CITY_Y   20
#define WEATHER_TEMP_Y   45
#define WEATHER_HILO_Y   65
#define WEATHER_LOW_X   380
#define CLOCK_X          80
#define CLOCK_Y          95
#define DATE_X           90
#define DIVIDER_Y       195
#define WIFI_ICON_X     450
#define WIFI_ICON_Y      15

// Light sensor vertical bar — left edge of screen
#define LUX_BAR_X       15
#define LUX_BAR_Y      210
#define LUX_BAR_WIDTH   14
#define LUX_BAR_HEIGHT  75
#define LUX_MAX        150  // tune to match your room lighting

// Local sensor readings — top left (SCD40 temp and humidity)
#define LOCAL_X         5
#define LOCAL_TEMP_Y    20
#define LOCAL_HUM_Y     44

// Particulate row — sits directly above the air quality row
// X values are canvas-local (canvas is pushed to screen at x=30)
#define PM_ROW_Y       262   // screen Y where the PM canvas is pushed
#define PM1_X            8
#define PM25_X         108
#define PM4_X          228
#define PM10_X         330

// Air quality row — bottom of screen (CO2 + VOC)
#define AQ_ROW_Y       292   // screen Y where the AQ canvas is pushed
#define AQ_CO2_X         8
#define AQ_VOC_X       200

// Menu geometry
#define MENU_X          20
#define MENU_Y          40
#define MENU_W         440
#define MENU_H         240
#define MENU_ITEM_H     35
#define MENU_FIRST_Y   105  // Y baseline of first menu item
#define MENU_CLOSE_X   430  // X close button X position
#define MENU_CLOSE_Y    55  // X close button Y position

// Brightness slider
#define SLIDER_X        40
#define SLIDER_Y       160
#define SLIDER_W       360
#define SLIDER_H        12
#define SLIDER_THUMB_R  14

// =============================================================================
// BRIGHTNESS
// =============================================================================
// Four steps: 25%, 50%, 75%, 100% — direct PWM, higher = brighter
const int BRIGHTNESS_STEPS[] = { 64, 127, 191, 255 };
const int BRIGHTNESS_COUNT   = 4;
const int AUTO_DIM_MIN       = 10;   // minimum PWM in darkness
const int AUTO_DIM_MAX       = 255;  // maximum PWM in bright light

// =============================================================================
// TIMING INTERVALS
// =============================================================================
const long clockInterval    =    100;   // 0.1s  — clock tick
const long wifiIconInterval =   5000;   // 5s    — WiFi icon refresh
const long sensorInterval   =   1000;   // 1s    — sensor poll
const long weatherInterval  = 900000;   // 15min — weather fetch
const long ntpInterval      = 3600000;  // 1hr   — NTP resync

unsigned long lastClockTime    = 0;
unsigned long lastWifiIconTime = 0;
unsigned long lastSensorTime   = 0;
unsigned long lastWeatherTime  = 0;
unsigned long lastNtpTime      = 0;

// =============================================================================
// OBJECTS
// =============================================================================
Arduino_DataBus  *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX      *tft = new Arduino_ST7796(bus, TFT_RST, 3); // rotation 3 = landscape
Adafruit_FT6206   touch;
Adafruit_TSL2591  tsl = Adafruit_TSL2591(2591);
SensirionI2cScd4x scd4x;
SensirionI2CSen5x sen5x;   // PM1/2.5/4/10 + VOC index, I2C addr 0x69
Preferences       prefs;
WebServer         server(80);   // dashboard is served at omnicore.local

// Off-screen canvases — draw here then push to screen in one shot to prevent flicker
GFXcanvas16 clockCanvas(420, 65);   // large — holds 24pt clock digits
GFXcanvas16 localCanvas(220, 55);   // top left — SCD40 temp and humidity
GFXcanvas16 pmCanvas(440, 24);      // particulates — PM1, PM2.5, PM4, PM10
GFXcanvas16 aqCanvas(440, 24);      // bottom — CO2 and VOC index
GFXcanvas16 dateCanvas(320, 24);    // date line — only redraws on minute change

// =============================================================================
// STATE
// =============================================================================
int  lastMin = -1, lastSec = -1;    // dirty bits — only redraw when changed
bool isNightMode          = false;
bool hasWeatherData       = false;   // gates weather display until first fetch
bool hasSCD40Data         = false;   // gates sensor display until first read
bool hasSEN54Data         = false;   // gates PM/VOC display until first valid read
bool isCelsius            = false;
bool isMilitaryTime       = false;
bool isAutoDim            = false;
int  brightness           = 3;       // index into BRIGHTNESS_STEPS
bool menuOpen             = false;
bool brightnessSliderOpen = false;
bool awaitingResetConfirm = false;
int  menuOffset           = 0;       // first visible menu item index

// Touch state — start position only, last position unreliable on FT6236 lift
bool          touchActive    = false;
int           touchStartX    = 0;
int           touchStartY    = 0;
unsigned long touchStartTime = 0;
unsigned long lastTapTime   = 0;
unsigned long lastTouchPoll = 0;

// Weather globals — written by fetchWeather(), read by updateWeatherDisplay()
float  g_currentTemp = 0;
float  g_tempHigh    = 0;
float  g_tempLow     = 0;
String g_skyStatus   = "";

// Sensor globals — written by updateLocalSensors(), read by updateSensorDisplay()
float    g_lux       = 0;
float    g_localTemp = 0;  // SCD40, stored as Fahrenheit, converted on display
float    g_humidity  = 0;  // SCD40
uint16_t g_co2       = 0;  // SCD40, ppm
float    g_pm1       = 0;  // SEN54, ug/m3
float    g_pm25      = 0;  // SEN54, ug/m3
float    g_pm4       = 0;  // SEN54, ug/m3
float    g_pm10      = 0;  // SEN54, ug/m3
float    g_voc       = 0;  // SEN54, index 0-500 (100 = your baseline)

// =============================================================================
// COLOR PALETTE — single source of truth for day and night mode colors
// =============================================================================
struct ColorPalette {
  uint16_t clock;
  uint16_t date;
  uint16_t ampm;
  uint16_t line;
  uint16_t city;
  uint16_t temp;
  uint16_t high;
  uint16_t low;
  uint16_t luxBar;
  uint16_t luxBg;
  uint16_t wifiActive;
  uint16_t wifiInactive;
  uint16_t co2Good;   // < 800 ppm
  uint16_t co2Warn;   // 800–1200 ppm
  uint16_t co2High;   // > 1200 ppm
  uint16_t pm25Good;  // < 12 ug/m3
  uint16_t pm25Warn;  // 12–35 ug/m3
  uint16_t pm25High;  // > 35 ug/m3
};

ColorPalette getColorPalette() {
  if (isNightMode) {
    return { 0xF800, 0x8000, 0xA000, 0x4000, 0x8000, 0xFBE0, 0xF800, 0x0410,
             0x8000, 0x2104, 0x0400, 0x4000,
             0x4000, 0x8400, 0x8000,
             0x4000, 0x8400, 0x8000 };
  } else {
    return {
      TFT_WHITE, TFT_LIGHTGREY, TFT_YELLOW,
      TFT_DARKGREY, TFT_LIGHTGREY, TFT_ORANGE,
      TFT_RED, TFT_CYAN,
      TFT_YELLOW, 0x2104, TFT_GREEN, TFT_RED,
      TFT_GREEN, TFT_YELLOW, TFT_RED,
      TFT_GREEN, TFT_YELLOW, TFT_RED
    };
  }
}

uint16_t getCO2Color(ColorPalette col) {
  if (g_co2 < 800)  return col.co2Good;
  if (g_co2 < 1200) return col.co2Warn;
  return col.co2High;
}

// PM2.5 is the health-relevant particulate number — EPA breakpoints in ug/m3
uint16_t getPM25Color(ColorPalette col) {
  if (g_pm25 < 12.0) return col.pm25Good;
  if (g_pm25 < 35.0) return col.pm25Warn;
  return col.pm25High;
}

// =============================================================================
// PROTOTYPES
// =============================================================================
void showSplashScreen();
void showSetupScreen(String apName);
void manageWiFiVault();
void connectToWiFi(String vSSID, String vPASS);
void launchSetupPortal();
void checkWiFiHealth();
void checkDaylightSavings();
void applyBrightness();
void loadSettings();
void saveSettings();
void checkTouch();
void handleMenuTouch(int x, int y);
void handleResetConfirmTouch(int x, int y);
void handleSliderTouch(int x, int y);
void openMenu();
void closeMenu();
void selectMenuItem(int itemIdx);
void drawMenu();
void drawScrollbar();
void drawBrightnessSlider();
void drawResetConfirm();
bool fetchWeather();
bool syncLocationAndTime();
void updateLocalSensors();
void updateTimeDisplay();
void updateWeatherDisplay();
void updateSensorDisplay();
void drawWiFiIcon(int x, int y);
void runKITTScanner(int x);
void handleRoot();
void handleData();
void startWebServer();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);

  // Backlight PWM — direct drive to display LED pin, higher = brighter
  ledcAttach(TFT_PWM, 5000, 8);
  loadSettings();
  applyBrightness();

  tft->begin();
  tft->invertDisplay(true); // ST7796 requires invert for correct colors
  tft->setRotation(3);
  tft->fillScreen(TFT_BLACK);

  showSplashScreen();

  // I2C bus — shared by TSL2591, SCD40, SEN54, and FT6236
  Wire.begin(TFT_SDA, TFT_SCL);
  Wire.setClock(400000); // 400kHz fast mode — reduces I2C transaction time

  // FT6236 hardware reset before init
  pinMode(CTP_RST, OUTPUT);
  digitalWrite(CTP_RST, LOW);
  delay(10);
  digitalWrite(CTP_RST, HIGH);
  delay(100);
  if (!touch.begin(40)) {
    Serial.println("[FT6206] Init failed");
  } else {
    Serial.println("[FT6206] Ready");
  }

  // TSL2591 light sensor
  tsl.begin();
  tsl.setGain(TSL2591_GAIN_LOW);
  tsl.setTiming(TSL2591_INTEGRATIONTIME_100MS);
  Serial.println("[TSL2591] Started");

  // TSL2591 leaves the I2C bus busy — reset it before talking to SCD40
  Wire.end();
  delay(50);
  Wire.begin(TFT_SDA, TFT_SCL);
  Wire.setClock(400000);

  // Re-init FT6236 after bus reset
  digitalWrite(CTP_RST, LOW);
  delay(10);
  digitalWrite(CTP_RST, HIGH);
  delay(100);
  touch.begin(40);

  // SCD40 — stop any previous session before starting fresh
  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  scd4x.stopPeriodicMeasurement();
  delay(200);
  uint16_t scd40Error = scd4x.startPeriodicMeasurement();
  if (scd40Error) Serial.println("[SCD40] Init failed");
  else            Serial.println("[SCD40] Started");

  // SEN54 — particulates + VOC index. Runs its own VOC algorithm internally,
  // so no external gas index library is needed. Fan spins up on startMeasurement().
  // PM readings take ~30s to settle after the fan starts.
  sen5x.begin(Wire);
  sen5x.deviceReset();
  delay(100);
  uint16_t sen54Error = sen5x.startMeasurement();
  if (sen54Error) Serial.println("[SEN54] Init failed");
  else            Serial.println("[SEN54] Started — fan spinning up");

  manageWiFiVault();
  syncLocationAndTime();
  startWebServer();

  tft->fillScreen(TFT_BLACK);

  // Arm watchdog after all blocking startup completes
  const esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = 30000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Armed — system ready");
}

// =============================================================================
// LOOP — coordinator, decides when each layer runs
// =============================================================================
void loop() {
  esp_task_wdt_reset();
  unsigned long currentMillis = millis();

  checkTouch(); // every loop — touch must feel instant
  server.handleClient();

  if (currentMillis - lastClockTime >= clockInterval) {
    checkWiFiHealth();
    if (!menuOpen) updateTimeDisplay();
    checkDaylightSavings();
    lastClockTime = currentMillis;
  }

  if (currentMillis - lastWifiIconTime >= wifiIconInterval) {
    if (!menuOpen) drawWiFiIcon(WIFI_ICON_X, WIFI_ICON_Y);
    lastWifiIconTime = currentMillis;
  }

  if (currentMillis - lastSensorTime >= sensorInterval) {
    updateLocalSensors();
    applyBrightness();
    if (!menuOpen) updateSensorDisplay();
    lastSensorTime = currentMillis;
  }

  if (WiFi.status() == WL_CONNECTED) {

    if (currentMillis - lastWeatherTime >= weatherInterval || lastWeatherTime == 0) {
      if (fetchWeather()) {
        if (!menuOpen) updateWeatherDisplay();
      } else {
        Serial.println("[Weather] Fetch failed — retry in 15min");
      }
      lastWeatherTime = currentMillis;
    }

    if (currentMillis - lastNtpTime >= ntpInterval) {
      if (syncLocationAndTime()) lastWeatherTime = 0;
      else Serial.println("[NTP] Sync failed — retry in 1hr");
      lastNtpTime = currentMillis;
    }

  }
}

// =============================================================================
// TOUCH INPUT
// =============================================================================

// Converts raw FT6236 coordinates to display coordinates
// Raw range: p.x = 0-319, p.y = 0-478
// Display:   x = 0-478 left to right, y = 0-319 top to bottom
void mapTouch(TS_Point p, int &x, int &y) {
  x = 478 - p.y;
  y = p.x;
}

void checkTouch() {
  // limit touch polling to 50Hz — reduces I2C bus contention with sensors
  unsigned long now = millis();
  if (now - lastTouchPoll < 20) return;
  lastTouchPoll = now;

  if (!touch.touched()) {
    if (touchActive) {
      touchActive = false;
      unsigned long holdTime = millis() - touchStartTime;

      // process as tap if short touch and not too soon after last tap
      if (holdTime > 10 && holdTime < 500 && millis() - lastTapTime > 400) {
        lastTapTime = millis();
        if (awaitingResetConfirm) {
          handleResetConfirmTouch(touchStartX, touchStartY);
        } else if (brightnessSliderOpen) {
          if (touchStartY < SLIDER_Y - 40 || touchStartY > SLIDER_Y + 60) {
            brightnessSliderOpen = false;
            saveSettings();
            drawMenu();
          }
        } else if (menuOpen) {
          handleMenuTouch(touchStartX, touchStartY);
        } else {
          openMenu();
        }
      }
    }
    return;
  }

  TS_Point p = touch.getPoint();
  int x, y;
  mapTouch(p, x, y);

  if (!touchActive) {
    touchActive    = true;
    touchStartX    = x;
    touchStartY    = y;
    touchStartTime = millis();
  }

  if (brightnessSliderOpen) {
    handleSliderTouch(x, y);
  }
}

void handleMenuTouch(int x, int y) {
  // X close button — top right corner of menu
  if (x > 415 && x < 455 && y > 42 && y < 72) {
    closeMenu();
    return;
  }

  // tap outside menu box closes it
  if (x < MENU_X || x > MENU_X + MENU_W || y < MENU_Y || y > MENU_Y + MENU_H) {
    closeMenu();
    return;
  }

  // up arrow button
  if (x > 335 && x < 370 && y > 253 && y < 278) {
    if (menuOffset > 0) {
      menuOffset--;
      drawMenu();
    }
    return;
  }

  // down arrow button
  if (x > 380 && x < 415 && y > 253 && y < 278) {
    if (menuOffset + 5 < 7) {
      menuOffset++;
      drawMenu();
    }
    return;
  }

  // check which item row was tapped
  for (int i = 0; i < 5; i++) {
    int itemIdx    = i + menuOffset;
    if (itemIdx >= 7) break;
    int itemTop    = MENU_FIRST_Y - 18 + (i * MENU_ITEM_H);
    int itemBottom = itemTop + MENU_ITEM_H;
    if (y >= itemTop && y <= itemBottom) {
      selectMenuItem(itemIdx);
      return;
    }
  }
}

void handleResetConfirmTouch(int x, int y) {
  // YES button
  if (x > 60 && x < 210 && y > 180 && y < 220) {
    awaitingResetConfirm = false;
    prefs.begin("wifi-gate", false); prefs.clear(); prefs.end();
    prefs.begin("settings",  false); prefs.clear(); prefs.end();
    WiFiManager wm; wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(2000);
    ESP.restart();
  }
  // NO button
  if (x > 270 && x < 420 && y > 180 && y < 220) {
    awaitingResetConfirm = false;
    drawMenu();
  }
}

void handleSliderTouch(int x, int y) {
  if (y < SLIDER_Y - 30 || y > SLIDER_Y + 50) return;

  int clampedX = constrain(x, SLIDER_X, SLIDER_X + SLIDER_W);
  int rawPwm   = map(clampedX, SLIDER_X, SLIDER_X + SLIDER_W, 64, 255);

  // find nearest brightness step
  int nearest = 0, minDiff = 999;
  for (int i = 0; i < BRIGHTNESS_COUNT; i++) {
    int diff = abs(rawPwm - BRIGHTNESS_STEPS[i]);
    if (diff < minDiff) { minDiff = diff; nearest = i; }
  }

  if (nearest != brightness) {
    brightness = nearest;
    applyBrightness();
    drawBrightnessSlider();
  }
}

// =============================================================================
// SETTINGS MENU
// =============================================================================

void openMenu() {
  menuOpen             = true;
  brightnessSliderOpen = false;
  awaitingResetConfirm = false;
  menuOffset           = 0;
  drawMenu();
}

void closeMenu() {
  menuOpen             = false;
  brightnessSliderOpen = false;
  awaitingResetConfirm = false;
  tft->fillScreen(TFT_BLACK);
  lastMin = -1;
  lastSec = -1; // force full dashboard redraw
  updateTimeDisplay();
  updateWeatherDisplay();
  updateSensorDisplay();
  drawWiFiIcon(WIFI_ICON_X, WIFI_ICON_Y);
}

void selectMenuItem(int itemIdx) {
  switch (itemIdx) {
    case 0: // Night Mode
      isNightMode = !isNightMode;
      saveSettings();
      drawMenu();
      break;

    case 1: // Temp Unit
      isCelsius = !isCelsius;
      saveSettings();
      drawMenu();
      break;

    case 2: // Military Time
      isMilitaryTime = !isMilitaryTime;
      saveSettings();
      drawMenu();
      break;

    case 3: // Brightness — open slider
      brightnessSliderOpen = true;
      drawBrightnessSlider();
      break;

    case 4: // Auto Dim
      isAutoDim = !isAutoDim;
      applyBrightness();
      saveSettings();
      drawMenu();
      break;

    case 5: // Reset WiFi
      awaitingResetConfirm = true;
      drawResetConfirm();
      break;

    case 6: // Close
      closeMenu();
      break;
  }
}

void drawScrollbar() {
  const int trackX = 450, trackY = 78, trackH = 190;
  const int visible = 5, itemCount = 7;

  int thumbH = (visible * trackH) / itemCount;
  int thumbY = trackY + (menuOffset * trackH) / itemCount;

  tft->fillRect(trackX, trackY, 4, trackH, 0x2104);
  tft->fillRect(trackX, thumbY, 4, thumbH, TFT_CYAN);
}

void drawMenu() {
  const char* items[] = {
    "Night Mode", "Temp Unit", "Military Time",
    "Brightness", "Auto Dim", "Reset WiFi", "Close"
  };
  const int itemCount = 7, visible = 5;

  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, 0x1082);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, TFT_CYAN);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(185, 65);
  tft->print("SETTINGS");
  tft->drawFastHLine(30, 75, 410, TFT_CYAN);

  tft->setTextColor(TFT_LIGHTGREY);
  tft->setCursor(MENU_CLOSE_X, MENU_CLOSE_Y);
  tft->print("X");

  for (int i = 0; i < visible; i++) {
    int itemIdx = i + menuOffset;
    if (itemIdx >= itemCount) break;
    int itemY = MENU_FIRST_Y + (i * MENU_ITEM_H);

    tft->fillRoundRect(30, itemY - 16, 410, MENU_ITEM_H - 2, 4, 0x0821);
    tft->setFont(&FreeSans9pt7b);
    tft->setTextColor(TFT_WHITE);
    tft->setCursor(50, itemY);
    tft->print(items[itemIdx]);

    tft->setCursor(340, itemY);
    if (itemIdx == 0) tft->print(isNightMode    ? "ON"  : "OFF");
    if (itemIdx == 1) tft->print(isCelsius      ? "C"   : "F");
    if (itemIdx == 2) tft->print(isMilitaryTime ? "ON"  : "OFF");
    if (itemIdx == 3) {
      int pct = map(BRIGHTNESS_STEPS[brightness], 64, 255, 25, 100);
      tft->printf("%d%%", pct);
    }
    if (itemIdx == 4) tft->print(isAutoDim ? "ON" : "OFF");

    tft->setTextColor(TFT_DARKGREY);
    tft->setCursor(420, itemY);
    tft->print(">");
  }

  drawScrollbar();

  // scroll buttons
  tft->fillRoundRect(335, 253, 35, 25, 4, 0x2104);
  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(344, 271);
  tft->print("^");

  tft->fillRoundRect(380, 253, 35, 25, 4, 0x2104);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(389, 271);
  tft->print("v");

  tft->setFont(NULL);
  tft->setTextColor(0x4208);
  tft->setCursor(30, 278);
  tft->print("Tap X to close");
}

void drawBrightnessSlider() {
  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, 0x1082);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, TFT_CYAN);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(170, 80);
  tft->print("BRIGHTNESS");
  tft->drawFastHLine(30, 90, 410, TFT_CYAN);

  int pct = map(BRIGHTNESS_STEPS[brightness], 64, 255, 25, 100);
  tft->setFont(&FreeSansBold18pt7b);
  tft->setTextColor(TFT_WHITE);
  tft->setCursor(200, 140);
  tft->printf("%d%%", pct);

  // track
  tft->fillRoundRect(SLIDER_X, SLIDER_Y, SLIDER_W, SLIDER_H, 6, 0x4208);
  // fill
  int fillW = map(pct, 25, 100, 0, SLIDER_W);
  tft->fillRoundRect(SLIDER_X, SLIDER_Y, fillW, SLIDER_H, 6, TFT_CYAN);
  // thumb
  int thumbX = SLIDER_X + fillW;
  tft->fillCircle(thumbX, SLIDER_Y + SLIDER_H / 2, SLIDER_THUMB_R, TFT_WHITE);
  tft->drawCircle(thumbX, SLIDER_Y + SLIDER_H / 2, SLIDER_THUMB_R, TFT_CYAN);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_DARKGREY);
  tft->setCursor(SLIDER_X, SLIDER_Y + 35);
  tft->print("25%");
  tft->setCursor(SLIDER_X + SLIDER_W - 25, SLIDER_Y + 35);
  tft->print("100%");

  tft->setFont(NULL);
  tft->setTextColor(0x4208);
  tft->setCursor(120, 220);
  tft->print("Drag to adjust  |  Tap outside to save & close");
}

void drawResetConfirm() {
  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, 0x1082);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, TFT_RED);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_WHITE);
  tft->setCursor(100, 100);
  tft->print("Reset WiFi credentials?");

  tft->setTextColor(TFT_LIGHTGREY);
  tft->setCursor(80, 130);
  tft->print("This will erase your saved WiFi and");
  tft->setCursor(80, 150);
  tft->print("restart the setup portal.");

  tft->fillRoundRect(60, 180, 150, 40, 6, TFT_RED);
  tft->setTextColor(TFT_WHITE);
  tft->setCursor(115, 205);
  tft->print("YES");

  tft->fillRoundRect(270, 180, 150, 40, 6, 0x2104);
  tft->drawRoundRect(270, 180, 150, 40, 6, TFT_CYAN);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(330, 205);
  tft->print("NO");
}

// =============================================================================
// DISPLAY LAYER — reads globals, draws to screen, never fetches data
// =============================================================================

void updateTimeDisplay() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  ColorPalette col = getColorPalette();

  if (timeinfo.tm_sec != lastSec) {

    if (timeinfo.tm_min != lastMin) {
      char dateBuf[25];
      strftime(dateBuf, 25, "%A, %b %d", &timeinfo);

      dateCanvas.fillScreen(TFT_BLACK);
      dateCanvas.setFont(&FreeSans9pt7b);
      dateCanvas.setTextColor(col.date);
      dateCanvas.setCursor(DATE_X - 60, 17);
      dateCanvas.print(dateBuf);

      tft->draw16bitRGBBitmap(60, 156, dateCanvas.getBuffer(), 320, 24);
      tft->drawFastHLine(60, DIVIDER_Y, 320, col.line);
      lastMin = timeinfo.tm_min;
    }

    clockCanvas.fillScreen(TFT_BLACK);
    clockCanvas.setFont(&FreeSansBold24pt7b);
    clockCanvas.setTextColor(col.clock);
    clockCanvas.setCursor(5, 50);

    if (isMilitaryTime) {
      clockCanvas.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      int hour12 = timeinfo.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      clockCanvas.printf("%d:%02d:%02d", hour12, timeinfo.tm_min, timeinfo.tm_sec);
      int endX = clockCanvas.getCursorX();
      clockCanvas.setFont(&FreeSans9pt7b);
      clockCanvas.setTextColor(col.ampm);
      clockCanvas.setCursor(endX + 6, 40);
      clockCanvas.print((timeinfo.tm_hour >= 12) ? "PM" : "AM");
    }

    tft->draw16bitRGBBitmap(CLOCK_X, CLOCK_Y, clockCanvas.getBuffer(), 420, 65);
    lastSec = timeinfo.tm_sec;
  }
}

void updateWeatherDisplay() {
  if (!hasWeatherData) return;

  ColorPalette col = getColorPalette();

  float displayTemp = isCelsius ? (g_currentTemp - 32) * 5.0 / 9.0 : g_currentTemp;
  float displayHigh = isCelsius ? (g_tempHigh    - 32) * 5.0 / 9.0 : g_tempHigh;
  float displayLow  = isCelsius ? (g_tempLow     - 32) * 5.0 / 9.0 : g_tempLow;
  const char* unit  = isCelsius ? "C" : "F";

  tft->fillRect(280, 5, 195, 80, TFT_BLACK);
  tft->setFont(&FreeSans9pt7b);

  tft->setTextColor(col.city);
  tft->setCursor(WEATHER_X, WEATHER_CITY_Y);
  tft->print(currentCity.substring(0, currentCity.indexOf(',')));

  tft->setTextColor(col.temp);
  tft->setCursor(WEATHER_X, WEATHER_TEMP_Y);
  tft->printf("%.0f%s %s", displayTemp, unit, g_skyStatus.c_str());

  tft->setTextColor(col.high);
  tft->setCursor(WEATHER_X, WEATHER_HILO_Y);
  tft->printf("H:%.0f", displayHigh);

  tft->setTextColor(col.low);
  tft->setCursor(WEATHER_LOW_X, WEATHER_HILO_Y);
  tft->printf("L:%.0f", displayLow);
}

void updateSensorDisplay() {
  ColorPalette col = getColorPalette();

  // lux bar — vertical fill proportional to ambient light
  int fillHeight = map(constrain((int)g_lux, 0, LUX_MAX), 0, LUX_MAX, 0, LUX_BAR_HEIGHT);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y, LUX_BAR_WIDTH, LUX_BAR_HEIGHT, col.luxBg);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y + (LUX_BAR_HEIGHT - fillHeight), LUX_BAR_WIDTH, fillHeight, col.luxBar);

  // SCD40 temp and humidity — top left, waits for first valid read
  if (hasSCD40Data) {
    float displayLocalTemp = isCelsius ? (g_localTemp - 32) * 5.0 / 9.0 : g_localTemp;
    const char* unit = isCelsius ? "C" : "F";

    localCanvas.fillScreen(TFT_BLACK);
    localCanvas.setFont(&FreeSans9pt7b);
    localCanvas.setTextColor(col.temp);
    localCanvas.setCursor(LOCAL_X, LOCAL_TEMP_Y);
    localCanvas.printf("%.1f%s", displayLocalTemp, unit);
    localCanvas.setTextColor(col.date);
    localCanvas.setCursor(LOCAL_X, LOCAL_HUM_Y);
    localCanvas.printf("HUM: %.0f%%", g_humidity);

    tft->draw16bitRGBBitmap(0, 5, localCanvas.getBuffer(), 220, 55);
  }

  // Particulate row — PM1 / PM2.5 / PM4 / PM10 from the SEN54.
  // PM2.5 is color coded since it is the health-relevant number.
  if (hasSEN54Data) {
    pmCanvas.fillScreen(TFT_BLACK);
    pmCanvas.setFont(&FreeSans9pt7b);

    pmCanvas.setTextColor(col.date);
    pmCanvas.setCursor(PM1_X, 17);
    pmCanvas.printf("PM1:%.1f", g_pm1);

    pmCanvas.setTextColor(getPM25Color(col)); // the one that matters
    pmCanvas.setCursor(PM25_X, 17);
    pmCanvas.printf("PM2.5:%.1f", g_pm25);

    pmCanvas.setTextColor(col.date);
    pmCanvas.setCursor(PM4_X, 17);
    pmCanvas.printf("PM4:%.1f", g_pm4);

    pmCanvas.setCursor(PM10_X, 17);
    pmCanvas.printf("PM10:%.1f", g_pm10);

    tft->draw16bitRGBBitmap(30, PM_ROW_Y, pmCanvas.getBuffer(), 440, 24);
  }

  // Air quality row — CO2 from SCD40, VOC index from SEN54.
  // Each is gated on its own sensor so one can appear before the other.
  if (hasSCD40Data || hasSEN54Data) {
    aqCanvas.fillScreen(TFT_BLACK);
    aqCanvas.setFont(&FreeSans9pt7b);

    if (hasSCD40Data) {
      aqCanvas.setTextColor(getCO2Color(col));
      aqCanvas.setCursor(AQ_CO2_X, 17);
      aqCanvas.printf("CO2:%dppm", g_co2);
    }

    if (hasSEN54Data) {
      aqCanvas.setTextColor(col.date);
      aqCanvas.setCursor(AQ_VOC_X, 17);
      aqCanvas.printf("VOC:%.0f", g_voc);
    }

    tft->draw16bitRGBBitmap(30, AQ_ROW_Y, aqCanvas.getBuffer(), 440, 24);
  }
}

void drawWiFiIcon(int x, int y) {
  ColorPalette col = getColorPalette();
  bool connected   = (WiFi.status() == WL_CONNECTED);
  int  bars        = !connected ? 0 : (WiFi.RSSI() > -60) ? 4 : (WiFi.RSSI() > -80) ? 2 : 1;
  uint16_t active  = connected ? col.wifiActive : col.wifiInactive;
  for (int i = 0; i < 4; i++) {
    tft->fillRect(x + (i * 6), y + (12 - (i * 3)), 4, 4 + (i * 3),
                  (i < bars) ? active : col.luxBg);
  }
}

void runKITTScanner(int x) {
  int y = 310, h = 4;
  int tailX = max(0, x - 15);
  tft->fillRect(0,     y, 480, h, TFT_BLACK);
  tft->fillRect(tailX, y,  40, h, 0x8000);
  tft->fillRect(x,     y,  10, h, TFT_RED);
}

// =============================================================================
// SYSTEM
// =============================================================================

void applyBrightness() {
  if (isAutoDim) {
    int pwm = map(constrain((int)g_lux, 0, LUX_MAX), 0, LUX_MAX, AUTO_DIM_MIN, AUTO_DIM_MAX);
    ledcWrite(TFT_PWM, pwm);
  } else {
    ledcWrite(TFT_PWM, BRIGHTNESS_STEPS[brightness]);
  }
}

void loadSettings() {
  prefs.begin("settings", true);
  isNightMode    = prefs.getBool("nightMode",    false);
  isCelsius      = prefs.getBool("celsius",      false);
  isMilitaryTime = prefs.getBool("militaryTime", false);
  isAutoDim      = prefs.getBool("autoDim",      false);
  brightness     = prefs.getInt ("brightness",   3);
  prefs.end();
  Serial.println("[NVS] Settings loaded");
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putBool("nightMode",    isNightMode);
  prefs.putBool("celsius",      isCelsius);
  prefs.putBool("militaryTime", isMilitaryTime);
  prefs.putBool("autoDim",      isAutoDim);
  prefs.putInt ("brightness",   brightness);
  prefs.end();
  Serial.println("[NVS] Settings saved");
}

void checkWiFiHealth() {
  static unsigned long disconnectTime     = 0;
  static unsigned long lastRetry          = 0;
  static unsigned long lastSuccessfulSync = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (disconnectTime == 0) disconnectTime = millis();

    if (millis() - lastRetry > 60000) {
      lastRetry = millis();
      prefs.begin("wifi-gate", false);
      String vSSID = prefs.getString("ssid", "");
      String vPASS = prefs.getString("pass", "");
      prefs.end();
      if (vSSID != "") WiFi.begin(vSSID.c_str(), vPASS.c_str());
    }

    if (millis() - disconnectTime > 21600000) ESP.restart(); // 6hr no connection

  } else {
    if (disconnectTime != 0) {
      if (millis() - lastSuccessfulSync > 60000) {
        syncLocationAndTime();
        lastSuccessfulSync = millis();
      }
      disconnectTime = 0;
    }
  }
}

void checkDaylightSavings() {
  static bool syncedThisMinute = false;
  struct tm ti;
  if (!getLocalTime(&ti)) return;

  if (ti.tm_hour == 2 && ti.tm_min == 1) {
    if (!syncedThisMinute && WiFi.status() == WL_CONNECTED) {
      if (syncLocationAndTime()) lastWeatherTime = 0;
      syncedThisMinute = true;
    }
  } else {
    syncedThisMinute = false;
  }
}

// =============================================================================
// WIFI MANAGEMENT
// =============================================================================

void manageWiFiVault() {
  prefs.begin("wifi-gate", false);
  String vSSID = prefs.getString("ssid", "");
  String vPASS = prefs.getString("pass", "");
  prefs.end();

  if (vSSID == "") launchSetupPortal();
  else             connectToWiFi(vSSID, vPASS);
}

void connectToWiFi(String vSSID, String vPASS) {
  tft->fillScreen(TFT_BLACK);
  tft->setFont(&FreeMono9pt7b);
  tft->setTextColor(TFT_YELLOW);
  tft->setCursor(10, 25);
  tft->print(OS_VERSION);

  tft->setTextColor(TFT_CYAN);
  tft->setCursor(20, 220);
  tft->print("> ESTABLISHING NEURAL LINK");

  tft->setTextColor(TFT_WHITE);
  tft->setCursor(20, 250);
  tft->print("  TARGET: ");
  tft->setTextColor(TFT_MAGENTA);
  tft->print(vSSID);

  tft->setFont(NULL);
  tft->setTextColor(0x4208);
  tft->setCursor(60, 290);
  tft->print("Hold screen 3s to reset WiFi");

  WiFi.begin(vSSID.c_str(), vPASS.c_str());

  int scannerX = 0, direction = 6;
  unsigned long lastRetry      = millis();
  unsigned long touchHoldStart = 0;

  while (WiFi.status() != WL_CONNECTED) {
    scannerX += direction;
    if (scannerX >= 470) direction = -6;
    if (scannerX <= 0)   direction =  6;
    runKITTScanner(scannerX);

    if (touch.touched()) {
      if (touchHoldStart == 0) touchHoldStart = millis();
      if (millis() - touchHoldStart > 3000) {
        prefs.begin("wifi-gate", false); prefs.clear(); prefs.end();
        WiFiManager wm; wm.resetSettings();
        WiFi.disconnect(true, true);
        delay(500);
        launchSetupPortal();
        return;
      }
    } else {
      touchHoldStart = 0;
    }

    if (millis() - lastRetry > 20000) {
      WiFi.begin(vSSID.c_str(), vPASS.c_str());
      lastRetry = millis();
    }
    delay(20);
  }

  tft->fillRect(15, 268, 450, 30, TFT_BLACK);
  tft->fillRect(0, 308, 480, 4, TFT_GREEN);
  tft->fillRect(15, 210, 450, 60, TFT_BLACK);
  tft->setTextColor(TFT_GREEN);
  tft->setCursor(20, 245);
  tft->print("> NEURAL LINK ESTABLISHED");

  delay(1000);
  tft->fillScreen(TFT_BLACK);
  tft->drawFastHLine(60, DIVIDER_Y, 320, TFT_DARKGREY);
}

void launchSetupPortal() {
  showSetupScreen("OmniCore-Setup");
  WiFiManager wm;
  wm.setTitle("Calloway OS Setup");

  wm.setCustomHeadElement("<style>body { background-color: #050505; color: #00FFFF; font-family: 'Courier New', monospace; margin: 0; display: flex; align-items: center; justify-content: center; min-height: 100vh; }.wrap { background: #0a0a0a; border: 2px solid #00FFFF; padding: 40px; box-shadow: 0 0 20px #00FFFF; width: 95% !important; max-width: 800px !important; }@media (min-width: 768px) {form { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: flex-end; }form div { width: 48%; }button { width: 100% !important; margin-top: 20px; }h1 { width: 100%; }}a { color: #00FFFF; text-decoration: none; font-weight: bold; padding: 12px; display: block; border-bottom: 1px solid #111; width: 100%; box-sizing: border-box; }a:hover { color: #00FF00; background: #001a1a; }img, svg, .q { filter: invert(48%) sepia(79%) saturate(2476%) hue-rotate(150deg) brightness(118%) contrast(119%); }label { color: #00FFFF; display: block; text-transform: uppercase; font-size: 0.75em; letter-spacing: 2px; margin-bottom: 5px; }input { background: #000; color: #00FF00; border: 1px solid #005555; padding: 12px; width: 100%; box-sizing: border-box; font-size: 16px; }input[type='checkbox'] { width: 18px !important; height: 18px !important; accent-color: #00FF00; display: inline-block !important; vertical-align: middle; margin: 10px 5px 10px 0 !important; }input[type='checkbox'] + label { display: inline-block !important; vertical-align: middle; width: auto !important; margin: 0 !important; font-size: 0.7em; letter-spacing: 1px; }button { background: #002222; border: 1px solid #00FFFF; color: #00FF00; padding: 15px; cursor: pointer; text-transform: uppercase; font-weight: bold; display: block; width: 100%; }button:hover { background: #00FFFF; color: #000; }h1, h2 { color: #00FF00; text-transform: uppercase; text-align: center; margin-top: 0; }</style>");

  wm.setSaveConfigCallback([]() {
    Preferences p; p.begin("wifi-gate", false);
    p.putString("ssid", WiFi.SSID());
    p.putString("pass", WiFi.psk());
    p.end();
  });

  if (!wm.startConfigPortal("OmniCore-Setup")) ESP.restart();
  ESP.restart();
}

// =============================================================================
// WEB SERVER — serves a live dashboard on the local network at omnicore.local
// Reads the same g_ globals the display does. Never fetches, never draws.
// =============================================================================

// Page lives in flash, not RAM. JS polls /data every 2s and patches the DOM,
// so the page never reloads and never flickers.
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Omni-Core</title>
<style>
  body{background:#050505;color:#0ff;font-family:'Courier New',monospace;
       margin:0;padding:20px;}
  h1{color:#0f0;text-align:center;letter-spacing:3px;margin:0 0 4px;font-size:1.4em;}
  .sub{text-align:center;color:#055;font-size:.75em;margin-bottom:24px;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));
        gap:12px;max-width:800px;margin:0 auto;}
  .card{background:#0a0a0a;border:1px solid #044;padding:14px;text-align:center;}
  .card:hover{border-color:#0ff;}
  .lbl{font-size:.65em;color:#077;text-transform:uppercase;letter-spacing:2px;}
  .val{font-size:1.7em;font-weight:bold;margin-top:6px;}
  .unit{font-size:.5em;color:#066;margin-left:3px;}
  .good{color:#0f0;} .warn{color:#ff0;} .bad{color:#f33;} .neut{color:#0ff;}
  .foot{text-align:center;color:#044;font-size:.65em;margin-top:24px;}
</style></head><body>
<h1>OMNI-CORE</h1>
<div class='sub' id='city'>&nbsp;</div>
<div class='grid'>
  <div class='card'><div class='lbl'>CO2</div>
    <div class='val neut' id='co2'>--<span class='unit'>ppm</span></div></div>
  <div class='card'><div class='lbl'>PM2.5</div>
    <div class='val neut' id='pm25'>--<span class='unit'>ug/m3</span></div></div>
  <div class='card'><div class='lbl'>VOC Index</div>
    <div class='val neut' id='voc'>--</div></div>
  <div class='card'><div class='lbl'>Temp</div>
    <div class='val neut' id='temp'>--<span class='unit'>F</span></div></div>
  <div class='card'><div class='lbl'>Humidity</div>
    <div class='val neut' id='hum'>--<span class='unit'>%</span></div></div>
  <div class='card'><div class='lbl'>Light</div>
    <div class='val neut' id='lux'>--<span class='unit'>lux</span></div></div>
  <div class='card'><div class='lbl'>PM1</div>
    <div class='val neut' id='pm1'>--</div></div>
  <div class='card'><div class='lbl'>PM4</div>
    <div class='val neut' id='pm4'>--</div></div>
  <div class='card'><div class='lbl'>PM10</div>
    <div class='val neut' id='pm10'>--</div></div>
</div>
<div class='foot'>CALLOWAY OS &middot; live &middot; updates every 2s</div>
<script>
function cls(el,c){el.className='val '+c;}
function tick(){
  fetch('/data').then(r=>r.json()).then(d=>{
    let co2=document.getElementById('co2');
    co2.innerHTML=d.co2+"<span class='unit'>ppm</span>";
    cls(co2, d.co2<800?'good':d.co2<1200?'warn':'bad');

    let pm=document.getElementById('pm25');
    pm.innerHTML=d.pm25.toFixed(1)+"<span class='unit'>ug/m3</span>";
    cls(pm, d.pm25<12?'good':d.pm25<35?'warn':'bad');

    document.getElementById('voc').textContent  = d.voc;
    document.getElementById('pm1').textContent  = d.pm1.toFixed(1);
    document.getElementById('pm4').textContent  = d.pm4.toFixed(1);
    document.getElementById('pm10').textContent = d.pm10.toFixed(1);
    document.getElementById('temp').innerHTML   = d.temp.toFixed(1)+"<span class='unit'>F</span>";
    document.getElementById('hum').innerHTML    = d.hum.toFixed(0)+"<span class='unit'>%</span>";
    document.getElementById('lux').innerHTML    = d.lux.toFixed(0)+"<span class='unit'>lux</span>";
    document.getElementById('city').textContent = d.city;
  }).catch(e=>{});
}
tick(); setInterval(tick,2000);
</script></body></html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

// Raw JSON — this is the reusable part. Any future app, script, or cloud
// integration can just read this endpoint.
void handleData() {
  char json[288];
  snprintf(json, sizeof(json),
    "{\"co2\":%u,\"pm1\":%.1f,\"pm25\":%.1f,\"pm4\":%.1f,\"pm10\":%.1f,"
    "\"voc\":%.0f,\"temp\":%.1f,\"hum\":%.1f,\"lux\":%.1f,\"city\":\"%s\"}",
    g_co2, g_pm1, g_pm25, g_pm4, g_pm10, g_voc,
    g_localTemp, g_humidity, g_lux,
    currentCity.substring(0, currentCity.indexOf(',')).c_str());
  server.send(200, "application/json", json);
}

void startWebServer() {
  if (MDNS.begin("omnicore")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[WEB] mDNS started — http://omnicore.local");
  }
  server.on("/",     handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.printf("[WEB] Dashboard live at http://%s  or  http://omnicore.local\n",
                WiFi.localIP().toString().c_str());
}

// =============================================================================
// DATA LAYER — fetches data, writes to globals, never touches the screen
// =============================================================================

bool fetchWeather() {
  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(3000);
  String url = "http://api.openweathermap.org/data/2.5/forecast?q=" + currentCity
               + "&appid=" + String(weatherKey) + "&units=imperial";
  bool success = false;
  if (http.begin(url)) {
    if (http.GET() == 200) {
      JsonDocument doc;
      deserializeJson(doc, http.getStream());
      g_currentTemp = doc["list"][0]["main"]["temp"];
      g_skyStatus   = doc["list"][0]["weather"][0]["main"].as<String>();
      g_tempHigh = -100.0;
      g_tempLow  =  200.0;
      for (int i = 0; i < 8; i++) {
        float h = doc["list"][i]["main"]["temp_max"];
        float l = doc["list"][i]["main"]["temp_min"];
        if (h > g_tempHigh) g_tempHigh = h;
        if (l < g_tempLow)  g_tempLow  = l;
      }
      hasWeatherData = true;
      success = true;
    }
    http.end();
  }
  return success;
}

bool syncLocationAndTime() {
  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(3000);
  bool success = false;
  if (http.begin("http://ip-api.com/json/?fields=status,city,countryCode,offset")) {
    if (http.GET() == 200) {
      JsonDocument doc;
      deserializeJson(doc, http.getString());
      if (String(doc["status"]) == "success") {
        currentUtcOffset = doc["offset"];
        currentCity = String(doc["city"]) + "," + String(doc["countryCode"]);
        configTime(currentUtcOffset, 0, "pool.ntp.org", "time.google.com");
        success = true;
      }
    }
    http.end();
  }
  return success;
}

void updateLocalSensors() {
  // TSL2591 — reject invalid readings
  uint32_t lum  = tsl.getFullLuminosity();
  uint16_t ir   = lum >> 16;
  uint16_t full = lum & 0xFFFF;
  if (full == 0 && ir == 0) {
    g_lux = 0;
  } else {
    float lux = tsl.calculateLux(full, ir);
    g_lux = (isnan(lux) || lux < 0 || lux > 88000) ? 0 : lux;
  }

  // SCD40 — only reads when sensor signals new data is ready
  uint16_t co2;
  float    temp, hum;
  bool     dataReady = false;
  scd4x.getDataReadyStatus(dataReady);
  if (dataReady) {
    uint16_t error = scd4x.readMeasurement(co2, temp, hum);
    if (!error && co2 != 0) {
      g_co2        = co2;
      g_localTemp  = temp * 9.0 / 5.0 + 32.0; // store as Fahrenheit
      g_humidity   = hum;
      hasSCD40Data = true;
    }
  }

  // SEN54 — particulates and VOC index.
  // The module runs its own compensation and VOC algorithm internally, so we
  // just read the finished values. It also reports its own temp/humidity, but
  // we ignore those and keep the SCD40 as the single source of truth for both.
  // SEN54 has no NOx sensor (that is the SEN55) so noxIndex comes back NaN.
  float senPm1, senPm25, senPm4, senPm10;
  float senHum, senTemp, senVoc, senNox;

  if (sen5x.readMeasuredValues(senPm1, senPm25, senPm4, senPm10,
                               senHum, senTemp, senVoc, senNox) == 0) {
    // fan needs ~30s before PM values are meaningful — reject NaN until then
    if (!isnan(senPm25) && !isnan(senVoc)) {
      g_pm1        = senPm1;
      g_pm25       = senPm25;
      g_pm4        = senPm4;
      g_pm10       = senPm10;
      g_voc        = senVoc;
      hasSEN54Data = true;
    }
  }

  Serial.printf("[LUX] %.1f  [CO2] %d ppm  [TEMP] %.1fF  [HUM] %.1f%%  "
                "[PM1] %.1f  [PM2.5] %.1f  [PM4] %.1f  [PM10] %.1f  [VOC] %.0f\n",
                g_lux, g_co2, g_localTemp, g_humidity,
                g_pm1, g_pm25, g_pm4, g_pm10, g_voc);
}

// =============================================================================
// SPLASH / SETUP SCREENS
// =============================================================================

void showSplashScreen() {
  tft->fillScreen(TFT_BLACK);
  tft->setFont(&FreeMono9pt7b);
  tft->setTextColor(TFT_YELLOW);
  tft->setCursor(10, 25);
  tft->print(OS_VERSION);
  tft->setFont(&FreeSansBold18pt7b);
  tft->setTextColor(TFT_ORANGE);
  tft->setCursor(130, 170);
  tft->print("OMNI-CORE");
}

void showSetupScreen(String apName) {
  tft->fillScreen(0x0010);
  tft->drawRoundRect(20, 20, 440, 280, 10, TFT_CYAN);
  tft->setFont(&FreeSansBold18pt7b);
  tft->setTextColor(TFT_YELLOW);
  tft->setCursor(140, 100);
  tft->print("SETUP MODE");
  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_WHITE);
  tft->setCursor(60, 160);
  tft->print("1. Connect Phone to WiFi:");
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(130, 190);
  tft->print(apName);
}
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
//  Version      : v1.9
//  Author       : Logan Calloway
//  License      : Copyright (c) 2025 Logan Calloway. All Rights Reserved.
//                 Unauthorized copying, distribution, or modification of
//                 this file, via any medium, is strictly prohibited.
// -----------------------------------------------------------------------------
//  Description  :
//    A desktop engineering dashboard built on the XIAO ESP32-C6. Pulls time
//    from NTP, weather from OpenWeatherMap, and reads live environmental data
//    from the TSL2591 (light), SCD40 (CO2, temp, humidity), and SGP41
//    (VOC index, NOx index). Full capacitive touch menu via FT6236. Settings
//    persist across reboots via NVS. Auto Dim drives the backlight from the
//    TSL2591 lux reading once the PFET is wired up.
// -----------------------------------------------------------------------------
//  Touch interaction :
//    Tap dashboard       — opens settings menu
//    Tap menu item       — toggles or acts immediately
//    Swipe up/down       — scrolls menu list
//    Tap X button        — closes menu
//    Brightness item     — opens slider, drag to adjust live
//    Reset WiFi item     — shows YES/NO confirmation screen
// -----------------------------------------------------------------------------
//  How it's organized :
//    DATA LAYER    — functions that fetch or calculate data, never draw
//    DISPLAY LAYER — functions that draw to the screen, never fetch
//    COORDINATOR   — loop() decides when everything runs and gates WiFi calls
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_FT6206.h>
#include <Adafruit_TSL2591.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>
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
#define TFT_BLUE        0x001F
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
#define OS_VERSION    "CALLOWAY_OS v1.9"
const char* weatherKey  = WEATHER_API_KEY;
String currentCity      = "Asheville,US";
long   currentUtcOffset = -14400;

// =============================================================================
// PIN MAPPING
// =============================================================================
#define TFT_RST   D0
#define TFT_CS    D1
#define TFT_PWM   D2  // backlight PWM — inverted when PFET is wired
#define TFT_DC    D3
#define TFT_SDA   D4  // I2C data
#define TFT_SCL   D5  // I2C clock
#define CTP_RST   D7  // touch controller reset
#define TFT_SCK   D8  // SPI clock
#define CTP_INT   D9  // touch interrupt
#define TFT_MOSI  D10 // SPI data

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
#define DATE_Y          175
#define DIVIDER_Y       195
#define WIFI_ICON_X     450
#define WIFI_ICON_Y      15

// Light sensor bar
#define LUX_BAR_X       15
#define LUX_BAR_Y      210
#define LUX_BAR_WIDTH   14
#define LUX_BAR_HEIGHT  90
#define LUX_MAX        150

// Local sensor readings — top left
#define LOCAL_X         5
#define LOCAL_TEMP_Y    20
#define LOCAL_HUM_Y     38

// Air quality — bottom of screen
#define AQ_CO2_X        40
#define AQ_CO2_Y       308
#define AQ_VOC_X       190
#define AQ_VOC_Y       308
#define AQ_NOX_X       330
#define AQ_NOX_Y       308

// Menu geometry
#define MENU_X          20
#define MENU_Y          40
#define MENU_W         440
#define MENU_H         240
#define MENU_ITEM_H     35
#define MENU_FIRST_Y   105 // Y of first item text baseline
#define MENU_CLOSE_X   430 // X button position
#define MENU_CLOSE_Y    55

// Brightness slider
#define SLIDER_X        40
#define SLIDER_Y       160
#define SLIDER_W       360
#define SLIDER_H        12
#define SLIDER_THUMB_R  14

// =============================================================================
// BRIGHTNESS CONSTANTS
// =============================================================================
// Steps the user cycles through — 25%, 50%, 75%, 100%
const int BRIGHTNESS_STEPS[] = { 64, 127, 191, 255 };
const int BRIGHTNESS_COUNT   = 4;
const int AUTO_DIM_MIN       = 10;  // dimmest auto dim will go
const int AUTO_DIM_MAX       = 255; // brightest auto dim will go

// =============================================================================
// WEATHER CONSTANTS
// =============================================================================
const int FORECAST_PERIODS_24H = 8;

// =============================================================================
// TIMING INTERVALS
// =============================================================================
const long clockInterval    =    100;
const long wifiIconInterval =   5000;
const long sensorInterval   =   1000;
const long weatherInterval  = 900000;
const long ntpInterval      = 3600000;

unsigned long lastClockTime    = 0;
unsigned long lastWifiIconTime = 0;
unsigned long lastSensorTime   = 0;
unsigned long lastWeatherTime  = 0;
unsigned long lastNtpTime      = 0;

// =============================================================================
// OBJECTS
// =============================================================================
Arduino_DataBus  *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX      *tft = new Arduino_ST7796(bus, TFT_RST, 3);
Adafruit_FT6206   touch;
Adafruit_TSL2591  tsl = Adafruit_TSL2591(2591);
SensirionI2cScd4x scd4x;
SensirionI2CSgp41 sgp41;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;
Preferences       prefs;

GFXcanvas16 clockCanvas(420, 65);  // bigger for 24pt font
GFXcanvas16 localCanvas(200, 45);
GFXcanvas16 aqCanvas(440, 22);
GFXcanvas16 dateCanvas(320, 22);

// =============================================================================
// STATE
// =============================================================================
int  lastMin = -1, lastSec = -1;
bool isNightMode          = false;
bool hasWeatherData       = false;
bool hasSCD40Data         = false;
bool hasSGP41Data         = false;
bool isCelsius            = false;
bool isMilitaryTime       = false;
bool isAutoDim            = false;
int  brightness           = 3;
bool menuOpen             = false;
bool brightnessSliderOpen = false; // true when brightness sub-screen is showing
bool awaitingResetConfirm = false;
int  menuOffset           = 0;

// Touch tracking
bool          touchActive     = false;
int           touchStartX     = 0;
int           touchStartY     = 0;
unsigned long touchStartTime  = 0;
int           lastTouchX      = 0;
int           lastTouchY      = 0;

// Weather globals
float  g_currentTemp = 0;
float  g_tempHigh    = 0;
float  g_tempLow     = 0;
String g_skyStatus   = "";

// Sensor globals
float    g_lux       = 0;
float    g_localTemp = 0;
float    g_humidity  = 0;
uint16_t g_co2       = 0;
int32_t  g_voc       = 0;
int32_t  g_nox       = 0;

// =============================================================================
// COLOR PALETTE
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
  uint16_t co2Good;
  uint16_t co2Warn;
  uint16_t co2High;
};

ColorPalette getColorPalette() {
  if (isNightMode) {
    return { 0xF800, 0x8000, 0xA000, 0x4000, 0x8000, 0xFBE0, 0xF800, 0x0410,
             0x8000, 0x2104, 0x0400, 0x4000,
             0x4000, 0x8400, 0x8000 };
  } else {
    return {
      TFT_WHITE, TFT_LIGHTGREY, TFT_YELLOW,
      TFT_DARKGREY, TFT_LIGHTGREY, TFT_ORANGE,
      TFT_RED, TFT_CYAN,
      TFT_YELLOW, 0x2104, TFT_GREEN, TFT_RED,
      TFT_GREEN, TFT_YELLOW, TFT_RED
    };
  }
}

uint16_t getCO2Color(ColorPalette col) {
  if (g_co2 < 800)  return col.co2Good;
  if (g_co2 < 1200) return col.co2Warn;
  return col.co2High;
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
void handleDashTouch(int x, int y);
void handleMenuTouch(int x, int y);
void handleResetConfirmTouch(int x, int y);
void handleSliderTouch(int x, int y);
void handleSwipe(int deltaY);
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

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);

  ledcAttach(TFT_PWM, 5000, 8);
  loadSettings();
  applyBrightness(); // uncomment when PFET is wired

  tft->begin();
  tft->invertDisplay(true);
  tft->setRotation(3);
  tft->fillScreen(TFT_BLACK);

  showSplashScreen();

  // I2C bus — shared by all sensors and FT6236 touch
  Wire.begin(TFT_SDA, TFT_SCL);

  // FT6236 capacitive touch
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

  // Reset I2C bus after TSL2591 — leaves bus busy which blocks SCD40
  Wire.end();
  delay(50);
  Wire.begin(TFT_SDA, TFT_SCL);

  // Re-init FT6236 after bus reset
  touch.begin(40);

  // SCD40
  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  scd4x.stopPeriodicMeasurement();
  delay(200);
  uint16_t scd40Error = scd4x.startPeriodicMeasurement();
  if (scd40Error) Serial.println("[SCD40] Init failed");
  else            Serial.println("[SCD40] Started");

  // SGP41
  sgp41.begin(Wire);
  uint16_t defaultRh = 0x8000;
  uint16_t defaultT  = 0x6666;
  uint16_t cond_s    = 0;
  sgp41.executeConditioning(defaultRh, defaultT, cond_s);
  Serial.println("[SGP41] Started");

  manageWiFiVault();
  syncLocationAndTime();

  tft->fillScreen(TFT_BLACK);

  const esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = 30000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Watchdog armed — system ready");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  esp_task_wdt_reset();
  unsigned long currentMillis = millis();

  checkTouch(); // runs every loop — touch must feel instant

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
        Serial.printf("[Weather] Fetch failed — retry in 15min\n");
      }
      lastWeatherTime = currentMillis;
    }

    if (currentMillis - lastNtpTime >= ntpInterval) {
      if (syncLocationAndTime()) lastWeatherTime = 0;
      else Serial.printf("[NTP] Sync failed — retry in 1hr\n");
      lastNtpTime = currentMillis;
    }

  }
}

// =============================================================================
// TOUCH INPUT
// =============================================================================

// Maps raw FT6236 coordinates to display coordinates
// Raw: X=0-319 (top to bottom), Y=0-478 (right to left)
// Mapped: 0,0 = top left, 480,320 = bottom right
void mapTouch(TS_Point p, int &x, int &y) {
  x = 478 - p.y;
  y = p.x;
}

// Main touch handler — runs every loop
void checkTouch() {
  if (!touch.touched()) {
    if (touchActive) {
      touchActive = false;

      // use start position only — last position from FT6236 is unreliable on lift
      unsigned long holdTime = millis() - touchStartTime;

      // only process as tap if it was a short touch — not a long hold or drag
      if (holdTime < 500) {
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
          handleDashTouch(touchStartX, touchStartY);
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

  lastTouchX = x;
  lastTouchY = y;

  if (brightnessSliderOpen) {
    handleSliderTouch(x, y);
  }
}

// Tap on dashboard — open menu
void handleDashTouch(int x, int y) {
  openMenu();
}

// Tap on menu — figure out what was tapped
void handleMenuTouch(int x, int y) {
  // X close button — top right of menu
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
    int itemIdx = i + menuOffset;
    if (itemIdx >= 7) break;
    int itemTop    = MENU_FIRST_Y - 18 + (i * MENU_ITEM_H);
    int itemBottom = itemTop + MENU_ITEM_H;
    if (y >= itemTop && y <= itemBottom) {
      selectMenuItem(itemIdx);
      return;
    }
  }
}

// Tap on YES/NO reset confirmation
void handleResetConfirmTouch(int x, int y) {
  // YES button — left side
  if (x > 60 && x < 210 && y > 180 && y < 220) {
    awaitingResetConfirm = false;
    prefs.begin("wifi-gate", false); prefs.clear(); prefs.end();
    prefs.begin("settings",  false); prefs.clear(); prefs.end();
    WiFiManager wm; wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(2000);
    ESP.restart();
  }
  // NO button — right side
  if (x > 270 && x < 420 && y > 180 && y < 220) {
    awaitingResetConfirm = false;
    drawMenu();
  }
}

// Live brightness slider drag
void handleSliderTouch(int x, int y) {
  if (y < SLIDER_Y - 30 || y > SLIDER_Y + 50) return;

  int clampedX = constrain(x, SLIDER_X, SLIDER_X + SLIDER_W);
  int rawPwm   = map(clampedX, SLIDER_X, SLIDER_X + SLIDER_W, 64, 255);

  // find nearest brightness step
  int nearest = 0;
  int minDiff = 999;
  for (int i = 0; i < BRIGHTNESS_COUNT; i++) {
    int diff = abs(rawPwm - BRIGHTNESS_STEPS[i]); // direct comparison, no inversion
    if (diff < minDiff) {
      minDiff = diff;
      nearest = i;
    }
  }

  if (nearest != brightness) {
    brightness = nearest;
    applyBrightness();
    drawBrightnessSlider();
  }
}

// Swipe up/down to scroll menu
void handleSwipe(int deltaY) {
  const int itemCount = 7;
  const int visible   = 5;
  if (deltaY < -30) {
    // swipe up — scroll down
    if (menuOffset + visible < itemCount) {
      menuOffset++;
      drawMenu();
    }
  } else if (deltaY > 30) {
    // swipe down — scroll up
    if (menuOffset > 0) {
      menuOffset--;
      drawMenu();
    }
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
  lastSec = -1;
  updateTimeDisplay();
  updateWeatherDisplay();
  updateSensorDisplay();
  drawWiFiIcon(WIFI_ICON_X, WIFI_ICON_Y);
}

// Called when a menu item row is tapped
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

    case 3: // Brightness — open slider sub-screen
      brightnessSliderOpen = true;
      drawBrightnessSlider();
      break;

    case 4: // Auto Dim
      isAutoDim = !isAutoDim;
      applyBrightness(); // uncomment when PFET is wired
      saveSettings();
      drawMenu();
      break;

    case 5: // Reset WiFi — show YES/NO confirmation
      awaitingResetConfirm = true;
      drawResetConfirm();
      break;

    case 6: // Close
      closeMenu();
      break;
  }
}

void drawScrollbar() {
  const int itemCount = 7;
  const int visible   = 5;
  const int trackX    = 450;
  const int trackY    = 78;
  const int trackH    = 190;

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
  const int itemCount = 7;
  const int visible   = 5;

  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, 0x1082);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, TFT_CYAN);

  // Title
  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(185, 65);
  tft->print("SETTINGS");
  tft->drawFastHLine(30, 75, 410, TFT_CYAN);

  // X close button — top right corner
  tft->setTextColor(TFT_LIGHTGREY);
  tft->setCursor(MENU_CLOSE_X, MENU_CLOSE_Y);
  tft->print("X");

  // Menu items
  for (int i = 0; i < visible; i++) {
    int itemIdx = i + menuOffset;
    if (itemIdx >= itemCount) break;
    int itemY = MENU_FIRST_Y + (i * MENU_ITEM_H);

    // highlight row
    tft->fillRoundRect(30, itemY - 16, 410, MENU_ITEM_H - 2, 4, 0x0821);
    tft->setFont(&FreeSans9pt7b);
    tft->setTextColor(TFT_WHITE);
    tft->setCursor(50, itemY);
    tft->print(items[itemIdx]);

    // value on right
    tft->setCursor(340, itemY);
    if (itemIdx == 0) tft->print(isNightMode    ? "ON"  : "OFF");
    if (itemIdx == 1) tft->print(isCelsius      ? "C"   : "F");
    if (itemIdx == 2) tft->print(isMilitaryTime ? "ON"  : "OFF");
    if (itemIdx == 3) {
      int pct = map(BRIGHTNESS_STEPS[brightness], 64, 255, 25, 100);
      tft->printf("%d%%", pct);
    }
    if (itemIdx == 4) tft->print(isAutoDim ? "ON" : "OFF");

    // tap hint arrow
    tft->setTextColor(TFT_DARKGREY);
    tft->setCursor(420, itemY);
    tft->print(">");
  }

  drawScrollbar();

  // up arrow button
  tft->fillRoundRect(335, 253, 35, 25, 4, 0x2104);
  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(344, 271);
  tft->print("^");

  // down arrow button
  tft->fillRoundRect(380, 253, 35, 25, 4, 0x2104);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(389, 271);
  tft->print("v");

  tft->setFont(NULL);
  tft->setTextColor(0x4208);
  tft->setCursor(30, 278);
  tft->print("Tap X to close");
}

// Brightness slider sub-screen — overlays the menu area
void drawBrightnessSlider() {
  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, 0x1082);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, TFT_CYAN);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(170, 80);
  tft->print("BRIGHTNESS");
  tft->drawFastHLine(30, 90, 410, TFT_CYAN);

  // current percentage label
  int pct = map(BRIGHTNESS_STEPS[brightness], 64, 255, 25, 100);
  tft->setFont(&FreeSansBold18pt7b);
  tft->setTextColor(TFT_WHITE);
  tft->setCursor(200, 140);
  tft->printf("%d%%", pct);

  // slider track
  tft->fillRoundRect(SLIDER_X, SLIDER_Y, SLIDER_W, SLIDER_H, 6, 0x4208);

  // slider fill — shows brightness level
  int fillW = map(pct, 25, 100, 0, SLIDER_W);
  tft->fillRoundRect(SLIDER_X, SLIDER_Y, fillW, SLIDER_H, 6, TFT_CYAN);

  // slider thumb
  int thumbX = SLIDER_X + fillW;
  tft->fillCircle(thumbX, SLIDER_Y + SLIDER_H / 2, SLIDER_THUMB_R, TFT_WHITE);
  tft->drawCircle(thumbX, SLIDER_Y + SLIDER_H / 2, SLIDER_THUMB_R, TFT_CYAN);

  // labels
  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_DARKGREY);
  tft->setCursor(SLIDER_X, SLIDER_Y + 35);
  tft->print("25%");
  tft->setCursor(SLIDER_X + SLIDER_W - 25, SLIDER_Y + 35);
  tft->print("100%");

  // instruction
  tft->setFont(NULL);
  tft->setTextColor(0x4208);
  tft->setCursor(120, 220);
  tft->print("Drag to adjust  |  Tap outside to save & close");
}

// WiFi reset confirmation with YES and NO buttons
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

  // YES button
  tft->fillRoundRect(60, 180, 150, 40, 6, TFT_RED);
  tft->setTextColor(TFT_WHITE);
  tft->setCursor(115, 205);
  tft->print("YES");

  // NO button
  tft->fillRoundRect(270, 180, 150, 40, 6, 0x2104);
  tft->drawRoundRect(270, 180, 150, 40, 6, TFT_CYAN);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(330, 205);
  tft->print("NO");
}

// =============================================================================
// DISPLAY LAYER
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

      tft->draw16bitRGBBitmap(60, 158, dateCanvas.getBuffer(), 320, 22);
      tft->drawFastHLine(60, DIVIDER_Y, 320, col.line);
      lastMin = timeinfo.tm_min;
    }

    clockCanvas.fillScreen(TFT_BLACK);
    clockCanvas.setFont(&FreeSansBold24pt7b); // big font for time digits
    clockCanvas.setTextColor(col.clock);
    clockCanvas.setCursor(5, 50);

    if (isMilitaryTime) {
      clockCanvas.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      int hour12 = timeinfo.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      clockCanvas.printf("%d:%02d:%02d", hour12, timeinfo.tm_min, timeinfo.tm_sec);
      int endX = clockCanvas.getCursorX();
      clockCanvas.setFont(&FreeSans9pt7b); // small font for AM/PM
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
  tft->setFont(&FreeMono9pt7b);

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

  // lux bar
  int fillHeight = map(constrain((int)g_lux, 0, LUX_MAX), 0, LUX_MAX, 0, LUX_BAR_HEIGHT);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y, LUX_BAR_WIDTH, LUX_BAR_HEIGHT, col.luxBg);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y + (LUX_BAR_HEIGHT - fillHeight), LUX_BAR_WIDTH, fillHeight, col.luxBar);

  // SCD40 — top left
  if (hasSCD40Data) {
    float displayLocalTemp = isCelsius ? (g_localTemp - 32) * 5.0 / 9.0 : g_localTemp;
    const char* unit = isCelsius ? "C" : "F";

    localCanvas.fillScreen(TFT_BLACK);
    localCanvas.setFont(&FreeMono9pt7b);
    localCanvas.setTextColor(col.temp);
    localCanvas.setCursor(LOCAL_X, LOCAL_TEMP_Y);
    localCanvas.printf("%.1f%s", displayLocalTemp, unit);
    localCanvas.setTextColor(col.date);
    localCanvas.setCursor(LOCAL_X, LOCAL_HUM_Y);
    localCanvas.printf("HUM: %.0f%%", g_humidity);

    tft->draw16bitRGBBitmap(0, 5, localCanvas.getBuffer(), 200, 45);
  }

  // Air quality — bottom
  if (hasSCD40Data) {
    aqCanvas.fillScreen(TFT_BLACK);
    aqCanvas.setFont(&FreeMono9pt7b);

    aqCanvas.setTextColor(getCO2Color(col));
    aqCanvas.setCursor(AQ_CO2_X - 30, 17);
    aqCanvas.printf("CO2:%dppm", g_co2);

    if (hasSGP41Data) {
      aqCanvas.setTextColor(col.date);
      aqCanvas.setCursor(AQ_VOC_X - 30, 17);
      aqCanvas.printf("VOC:%d", g_voc);
      aqCanvas.setCursor(AQ_NOX_X - 30, 17);
      aqCanvas.printf("NOx:%d", g_nox);
    }

    tft->draw16bitRGBBitmap(30, 296, aqCanvas.getBuffer(), 440, 22);
  }
}

void drawWiFiIcon(int x, int y) {
  ColorPalette col  = getColorPalette();
  bool connected    = (WiFi.status() == WL_CONNECTED);
  int  bars         = !connected ? 0 : (WiFi.RSSI() > -60) ? 4 : (WiFi.RSSI() > -80) ? 2 : 1;
  uint16_t active   = connected ? col.wifiActive : col.wifiInactive;
  for (int i = 0; i < 4; i++) {
    tft->fillRect(x + (i * 6), y + (12 - (i * 3)), 4, 4 + (i * 3),
                  (i < bars) ? active : col.luxBg);
  }
}

void runKITTScanner(int x) {
  int y     = 310, h = 4;
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

    if (millis() - disconnectTime > 21600000) ESP.restart();

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

  // hint visible from the start so user knows the escape hatch exists
  tft->setFont(NULL);
  tft->setTextColor(0x4208); // dim grey
  tft->setCursor(60, 290);
  tft->print("Hold screen 3s to reset WiFi");

  WiFi.begin(vSSID.c_str(), vPASS.c_str());

  int scannerX       = 0;
  int direction      = 6;
  unsigned long lastRetry      = millis();
  unsigned long touchHoldStart = 0;

  while (WiFi.status() != WL_CONNECTED) {
    scannerX += direction;
    if (scannerX >= 470) direction = -6;
    if (scannerX <= 0)   direction =  6;
    runKITTScanner(scannerX);

    // hold screen for 3 seconds to trigger WiFi reset
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
      touchHoldStart = 0; // finger lifted — reset timer
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
// DATA LAYER
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
      for (int i = 0; i < FORECAST_PERIODS_24H; i++) {
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
  // TSL2591 lux
  uint32_t lum  = tsl.getFullLuminosity();
  uint16_t ir   = lum >> 16;
  uint16_t full = lum & 0xFFFF;
  if (full == 0 && ir == 0) {
    g_lux = 0;
  } else {
    float lux = tsl.calculateLux(full, ir);
    g_lux = (isnan(lux) || lux < 0 || lux > 88000) ? 0 : lux;
  }

  // SCD40
  uint16_t co2;
  float    temp, hum;
  bool     dataReady = false;
  scd4x.getDataReadyStatus(dataReady);
  if (dataReady) {
    uint16_t error = scd4x.readMeasurement(co2, temp, hum);
    if (!error && co2 != 0) {
      g_co2        = co2;
      g_localTemp  = temp * 9.0 / 5.0 + 32.0;
      g_humidity   = hum;
      hasSCD40Data = true;
    }
  }

  // SGP41
  uint16_t srawVoc = 0, srawNox = 0;
  uint16_t rhComp  = hasSCD40Data ? (uint16_t)(g_humidity * 65535.0 / 100.0) : 0x8000;
  uint16_t tComp   = hasSCD40Data ? (uint16_t)(((g_localTemp - 32.0) * 5.0 / 9.0 + 45.0) * 65535.0 / 175.0) : 0x6666;

  if (sgp41.measureRawSignals(rhComp, tComp, srawVoc, srawNox) == 0) {
    g_voc        = vocAlgorithm.process(srawVoc);
    g_nox        = noxAlgorithm.process(srawNox);
    hasSGP41Data = true;
  }

  Serial.printf("[LUX] %.1f  [CO2] %d ppm  [TEMP] %.1fF  [HUM] %.1f%%  [VOC] %d  [NOx] %d\n",
                g_lux, g_co2, g_localTemp, g_humidity, g_voc, g_nox);
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
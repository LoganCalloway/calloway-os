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
//  Version      : v1.8
//  Author       : Logan Calloway
//  License      : Copyright (c) 2025 Logan Calloway. All Rights Reserved.
//                 Unauthorized copying, distribution, or modification of
//                 this file, via any medium, is strictly prohibited.
// -----------------------------------------------------------------------------
//  Description  :
//    A desktop engineering dashboard built on the XIAO ESP32-C6. Pulls time
//    from NTP, weather from OpenWeatherMap, and reads live environmental data
//    from the TSL2591 (light), SCD40 (CO2, temp, humidity), and SGP41
//    (VOC index, NOx index). Has a scrollable settings menu navigated by a
//    single hardware button. All settings persist across reboots via NVS.
//    Auto Dim drives the backlight from the TSL2591 lux reading once the
//    PFET is wired up. Touch support (FT6236) coming in stage 2.
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
#include <Adafruit_TSL2591.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

#include "secrets.h"

// =============================================================================
// COLOR DEFINITIONS — replaces ILI9341_ constants
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
#define OS_VERSION    "CALLOWAY_OS v1.8"
const char* weatherKey  = WEATHER_API_KEY;
String currentCity      = "Asheville,US";   // gets updated by syncLocationAndTime()
long   currentUtcOffset = -14400;           // gets updated by syncLocationAndTime()

// =============================================================================
// PIN MAPPING
// =============================================================================
#define TFT_RST   D0
#define TFT_CS    D1
#define TFT_PWM   D2  // backlight PWM — inverted when PFET is wired (255 = off, 0 = full)
#define TFT_DC    D3
#define TFT_SDA   D4  // I2C data
#define TFT_SCL   D5  // I2C clock
#define RESET_PIN D6  // settings button — deep reset only once touch is active
#define CTP_RST   D7  // touch controller reset
#define TFT_SCK   D8  // SPI clock
#define CTP_INT   D9  // touch interrupt
#define TFT_MOSI  D10 // SPI data

// =============================================================================
// LAYOUT CONSTANTS — change these to move things around on screen
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
#define LUX_MAX        400  // tweak to match your room — 400 works well indoors

// Local sensor readings — top left (SCD40 temp and humidity)
#define LOCAL_X         5
#define LOCAL_TEMP_Y    20
#define LOCAL_HUM_Y     38

// Air quality readings — bottom of screen
#define AQ_CO2_X        40
#define AQ_CO2_Y       308
#define AQ_VOC_X       190
#define AQ_VOC_Y       308
#define AQ_NOX_X       330
#define AQ_NOX_Y       308

// =============================================================================
// BRIGHTNESS CONSTANTS
// =============================================================================
// Steps the user cycles through in the menu — 25%, 50%, 75%, 100%
// Inverted for PFET: high PWM = gate pulled toward source = FET more off = dimmer
const int BRIGHTNESS_STEPS[] = { 191, 127, 63, 0 };
const int BRIGHTNESS_COUNT   = 4;
const int AUTO_DIM_MIN       = 191; // dimmest auto dim will go
const int AUTO_DIM_MAX       = 0;   // brightest auto dim will go

// =============================================================================
// WEATHER CONSTANTS
// =============================================================================
const int FORECAST_PERIODS_24H = 8; // 8 x 3hr intervals = 24hr lookahead

// =============================================================================
// TIMING INTERVALS
// =============================================================================
const long clockInterval    =    100;   // 0.1s  — clock redraw
const long wifiIconInterval =   5000;   // 5s    — WiFi icon redraw
const long sensorInterval   =   1000;   // 1s    — check sensors, SCD40 controls its own timing
const long weatherInterval  = 900000;   // 15min — weather fetch
const long ntpInterval      = 3600000;  // 1h    — NTP resync

unsigned long lastClockTime    = 0;
unsigned long lastWifiIconTime = 0;
unsigned long lastSensorTime   = 0;
unsigned long lastWeatherTime  = 0;
unsigned long lastNtpTime      = 0;

// =============================================================================
// OBJECTS
// =============================================================================
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX     *tft = new Arduino_ST7796(bus, TFT_RST, 3); // 3 = landscape, invertDisplay called in setup
Adafruit_TSL2591     tsl = Adafruit_TSL2591(2591);
SensirionI2cScd4x    scd4x;
SensirionI2CSgp41    sgp41;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;
Preferences          prefs;
GFXcanvas16          clockCanvas(420, 65); // clock buffer — wider for bigger display
GFXcanvas16          localCanvas(200, 45); // local sensor buffer — temp and humidity
GFXcanvas16          aqCanvas(440, 22);    // air quality buffer — CO2, VOC, NOx
GFXcanvas16          dateCanvas(320, 22);  // date buffer — prevents flicker on minute updates

// =============================================================================
// STATE
// =============================================================================
int  lastMin = -1, lastSec = -1;  // only redraw when something actually changed
bool isNightMode          = false;
bool hasWeatherData       = false; // don't draw weather until we have real data
bool hasSCD40Data         = false; // don't draw local sensors until first read
bool hasSGP41Data         = false; // don't draw air quality until warmup completes
bool isCelsius            = false; // temp unit — false = Fahrenheit
bool isMilitaryTime       = false; // 24hr clock toggle
bool isAutoDim            = false; // auto dim from TSL2591 — activates when PFET is wired
int  brightness           = 3;    // index into BRIGHTNESS_STEPS[] — 3 = full brightness
bool menuOpen             = false; // is the settings menu showing
bool awaitingResetConfirm = false; // waiting for user to confirm WiFi reset
int  menuIndex            = 0;    // which menu item is currently highlighted
int  menuOffset           = 0;    // first visible menu item — drives scrolling
bool brightnessSelected   = false; // true when user is in brightness adjust mode

// Weather globals — fetchWeather() writes these, updateWeatherDisplay() reads them
float  g_currentTemp = 0;
float  g_tempHigh    = 0;
float  g_tempLow     = 0;
String g_skyStatus   = "";

// Sensor globals — updateLocalSensors() writes these, updateSensorDisplay() reads them
float    g_lux       = 0;
float    g_localTemp = 0; // SCD40 temp, stored as Fahrenheit
float    g_humidity  = 0; // SCD40 humidity
uint16_t g_co2       = 0; // SCD40 CO2 in ppm
int32_t  g_voc       = 0; // SGP41 VOC index 0-500
int32_t  g_nox       = 0; // SGP41 NOx index 0-500

// =============================================================================
// COLOR PALETTE — one place for all colors, works for both day and night mode
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
  uint16_t luxBar;       // light bar fill color
  uint16_t luxBg;        // light bar background
  uint16_t wifiActive;   // WiFi icon when connected
  uint16_t wifiInactive; // WiFi icon when disconnected
  uint16_t co2Good;      // CO2 color when levels are normal
  uint16_t co2Warn;      // CO2 color when levels are elevated
  uint16_t co2High;      // CO2 color when levels are high
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

// Returns the right CO2 color based on the current reading
// Good: <800ppm  Warn: 800-1200ppm  High: >1200ppm
uint16_t getCO2Color(ColorPalette col) {
  if (g_co2 < 800)  return col.co2Good;
  if (g_co2 < 1200) return col.co2Warn;
  return col.co2High;
}

// =============================================================================
// PROTOTYPES
// =============================================================================

// Splash / setup screens
void showSplashScreen();
void showSetupScreen(String apName);

// WiFi management
void manageWiFiVault();
void connectToWiFi(String vSSID, String vPASS);
void launchSetupPortal();

// System
void handleDeepReset();
void checkUserButton();
void checkWiFiHealth();
void checkDaylightSavings();
void applyBrightness();
void loadSettings();
void saveSettings();

// Button input
void onShortPress();
void onLongPress();

// Settings menu
void openMenu();
void closeMenu();
void navigateMenu();
void selectMenuItem();
void drawMenu();
void drawScrollbar();

// DATA LAYER — fetch/calculate only, never touch the display
bool fetchWeather();
bool syncLocationAndTime();
void updateLocalSensors();

// DISPLAY LAYER — read globals and draw only, never fetch
void updateTimeDisplay();
void updateWeatherDisplay();
void updateSensorDisplay();
void drawWiFiIcon(int x, int y);
void drawDynamicBorder(float temp);
void runKITTScanner(int x);

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);

  ledcAttach(TFT_PWM, 5000, 8);

  // Load saved settings before anything draws so the first frame is correct
  loadSettings();
  // applyBrightness(); // uncomment when PFET is wired

  tft->begin();
  tft->invertDisplay(true); // ST7796 requires invert for correct colors
  tft->setRotation(3);
  tft->fillScreen(TFT_BLACK);

  showSplashScreen();

  // I2C bus — shared by TSL2591, SCD40, and SGP41
  Wire.begin(TFT_SDA, TFT_SCL);

  // TSL2591 light sensor
  tsl.begin();
  tsl.setGain(TSL2591_GAIN_LOW);
  tsl.setTiming(TSL2591_INTEGRATIONTIME_100MS);
  Serial.println("[TSL2591] Started");

  // Reset I2C bus after TSL2591 — TSL2591 leaves bus busy which blocks SCD40
  Wire.end();
  delay(50);
  Wire.begin(TFT_SDA, TFT_SCL);

  // SCD40 CO2, temperature, and humidity sensor
  // stop any previous measurement before starting fresh
  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  scd4x.stopPeriodicMeasurement();
  delay(200);
  uint16_t scd40Error = scd4x.startPeriodicMeasurement();
  if (scd40Error) {
    Serial.println("[SCD40] Init failed");
  } else {
    Serial.println("[SCD40] Started");
  }

  // SGP41 VOC and NOx sensor — conditioning run before first measurement
  sgp41.begin(Wire);
  uint16_t defaultRh      = 0x8000; // 50% RH default during conditioning
  uint16_t defaultT       = 0x6666; // 25C default during conditioning
  uint16_t conditioning_s = 0;
  sgp41.executeConditioning(defaultRh, defaultT, conditioning_s);
  Serial.println("[SGP41] Started");

  handleDeepReset();
  manageWiFiVault();     // blocks here until connected — watchdog not armed yet
  syncLocationAndTime();

  tft->fillScreen(TFT_BLACK);

  // Watchdog armed after all blocking startup is done.
  // If loop() stalls for more than 30s the chip resets itself.
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
// LOOP — decides when everything runs and gates all network calls behind WiFi
// =============================================================================
void loop() {
  esp_task_wdt_reset();
  unsigned long currentMillis = millis();

  checkUserButton(); // runs every loop so the button always feels instant

  if (currentMillis - lastClockTime >= clockInterval) {
    checkWiFiHealth();
    if (!menuOpen) updateTimeDisplay(); // don't draw over the menu
    checkDaylightSavings();
    lastClockTime = currentMillis;
  }

  if (currentMillis - lastWifiIconTime >= wifiIconInterval) {
    if (!menuOpen) drawWiFiIcon(WIFI_ICON_X, WIFI_ICON_Y);
    lastWifiIconTime = currentMillis;
  }

  if (currentMillis - lastSensorTime >= sensorInterval) {
    updateLocalSensors();              // always read — menu doesn't pause sensors
    // if (isAutoDim) applyBrightness(); // uncomment when PFET is wired
    if (!menuOpen) updateSensorDisplay();
    lastSensorTime = currentMillis;
  }

  if (WiFi.status() == WL_CONNECTED) { // all network calls gated here

    if (currentMillis - lastWeatherTime >= weatherInterval || lastWeatherTime == 0) {
      if (fetchWeather()) {
        if (!menuOpen) updateWeatherDisplay(); // only draw if menu is closed
      } else {
        Serial.printf("[Weather] Fetch failed — retry in 15min\n");
      }
      lastWeatherTime = currentMillis;
    }

    if (currentMillis - lastNtpTime >= ntpInterval) {
      if (syncLocationAndTime()) lastWeatherTime = 0; // location may have changed — grab fresh weather
      else Serial.printf("[NTP] Sync failed — retry in 1hr\n");
      lastNtpTime = currentMillis;
    }

  }
}

// =============================================================================
// DISPLAY LAYER — reads globals and draws to screen, never fetches anything
// =============================================================================

void updateTimeDisplay() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  ColorPalette col = getColorPalette();

  if (timeinfo.tm_sec != lastSec) {

    if (timeinfo.tm_min != lastMin) { // only redraw the date when the minute changes
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
    clockCanvas.setFont(&FreeSansBold24pt7b); // big font for the time digits
    clockCanvas.setTextColor(col.clock);
    clockCanvas.setCursor(5, 50);

    if (isMilitaryTime) {
      // 24hr — no AM/PM
      clockCanvas.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      // 12hr — convert hour and draw AM/PM small after
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

  // convert temps if user switched to Celsius — globals always store Fahrenheit
  float displayTemp = isCelsius ? (g_currentTemp - 32) * 5.0 / 9.0 : g_currentTemp;
  float displayHigh = isCelsius ? (g_tempHigh    - 32) * 5.0 / 9.0 : g_tempHigh;
  float displayLow  = isCelsius ? (g_tempLow     - 32) * 5.0 / 9.0 : g_tempLow;
  const char* unit  = isCelsius ? "C" : "F";

  tft->fillRect(280, 5, 195, 80, TFT_BLACK);
  tft->setFont(&FreeMono9pt7b);

  tft->setTextColor(col.city);
  tft->setCursor(WEATHER_X, WEATHER_CITY_Y);
  tft->print(currentCity.substring(0, currentCity.indexOf(','))); // drop the country code

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

// Draws lux bar, SCD40 local readings, and SGP41 air quality
void updateSensorDisplay() {
  ColorPalette col = getColorPalette();

  // lux bar — always draws
  int fillHeight = map(constrain((int)g_lux, 0, LUX_MAX), 0, LUX_MAX, 0, LUX_BAR_HEIGHT);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y, LUX_BAR_WIDTH, LUX_BAR_HEIGHT, col.luxBg);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y + (LUX_BAR_HEIGHT - fillHeight), LUX_BAR_WIDTH, fillHeight, col.luxBar);

  // SCD40 — top left, temp and humidity, draws as soon as first read succeeds
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

  // Air quality — bottom of screen
  // CO2 from SCD40 shows as soon as data arrives
  // VOC and NOx from SGP41 wait for warmup
  if (hasSCD40Data) {
    aqCanvas.fillScreen(TFT_BLACK);
    aqCanvas.setFont(&FreeMono9pt7b);

    // CO2 — color changes based on level
    aqCanvas.setTextColor(getCO2Color(col));
    aqCanvas.setCursor(AQ_CO2_X - 30, 15);
    aqCanvas.printf("CO2:%dppm", g_co2);

    // VOC and NOx — only show after SGP41 warmup
    if (hasSGP41Data) {
      aqCanvas.setTextColor(col.date);
      aqCanvas.setCursor(AQ_VOC_X - 30, 15);
      aqCanvas.printf("VOC:%d", g_voc);

      aqCanvas.setCursor(AQ_NOX_X - 30, 15);
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

void drawDynamicBorder(float temp) { // border color reflects outside temperature
  uint16_t borderColor;
  if      (temp >= 85) borderColor = TFT_RED;
  else if (temp >= 70) borderColor = TFT_ORANGE;
  else if (temp >= 55) borderColor = TFT_GREEN;
  else if (temp >= 40) borderColor = 0x07FF; // cyan
  else                 borderColor = TFT_BLUE;

  tft->drawRect(0, 0, 320, 240, borderColor);
  tft->drawRect(1, 1, 318, 238, borderColor); // double up for a bolder look
}

void runKITTScanner(int x) { // animated loading bar during WiFi connect
  int y     = 310, h = 4;
  int tailX = max(0, x - 15);
  tft->fillRect(0,     y, 480, h, TFT_BLACK);
  tft->fillRect(tailX, y,  40, h, 0x8000);
  tft->fillRect(x,     y,  10, h, TFT_RED);
}

// =============================================================================
// SETTINGS MENU
// =============================================================================

// Opens the menu and draws it from the top
void openMenu() {
  menuOpen           = true;
  menuIndex          = 0;
  menuOffset         = 0;
  brightnessSelected = false;
  drawMenu();
}

// Closes the menu and redraws the full dashboard
void closeMenu() {
  menuOpen           = false;
  brightnessSelected = false;
  tft->fillScreen(TFT_BLACK);
  lastMin = -1;
  lastSec = -1; // force a full dashboard redraw
  updateTimeDisplay();
  updateWeatherDisplay();
  updateSensorDisplay();
  drawWiFiIcon(WIFI_ICON_X, WIFI_ICON_Y);
}

// Short press moves the highlight to the next item
// If in brightness adjust mode, short press cycles brightness instead
void navigateMenu() {
  if (brightnessSelected) {
    brightness = (brightness + 1) % BRIGHTNESS_COUNT; // cycle through brightness steps
    // applyBrightness(); // uncomment when PFET is wired
    drawMenu();
    return;
  }
  menuIndex = (menuIndex + 1) % 7;
  if (menuIndex >= menuOffset + 5) menuOffset++; // 5 visible items on bigger screen
  if (menuIndex == 0) menuOffset = 0;
  drawMenu();
}

// Long press acts on whatever is currently highlighted
void selectMenuItem() {
  switch (menuIndex) {
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

    case 3: // Brightness — long press enters/exits adjust mode
      brightnessSelected = !brightnessSelected;
      if (!brightnessSelected) saveSettings(); // save when exiting adjust mode
      drawMenu();
      break;

    case 4: // Auto Dim
      isAutoDim = !isAutoDim;
      // applyBrightness(); // uncomment when PFET is wired
      saveSettings();
      drawMenu();
      break;

    case 5: // Reset WiFi — show confirmation before doing anything
      awaitingResetConfirm = true;
      tft->fillScreen(TFT_BLACK);
      tft->drawRoundRect(40, 100, 400, 120, 8, TFT_RED);
      tft->setFont(&FreeSans9pt7b);
      tft->setTextColor(TFT_WHITE);
      tft->setCursor(100, 140);
      tft->print("Reset WiFi credentials?");
      tft->setTextColor(TFT_LIGHTGREY);
      tft->setCursor(130, 165);
      tft->print("Hold to confirm.");
      tft->setCursor(130, 188);
      tft->print("Tap to cancel.");
      break;

    case 6: closeMenu(); break;
  }
}

// Draws the scrollbar — thumb moves down as you scroll through items
void drawScrollbar() {
  const int itemCount = 7;
  const int visible   = 5;
  const int trackX    = 450;
  const int trackY    = 68;
  const int trackH    = 210;

  int thumbH = (visible * trackH) / itemCount; // thumb height proportional to visible ratio
  int thumbY = trackY + (menuOffset * trackH) / itemCount; // moves down as menuOffset increases

  tft->fillRect(trackX, trackY, 4, trackH, 0x2104);       // dim grey track
  tft->fillRect(trackX, thumbY, 4, thumbH, TFT_CYAN); // cyan thumb
}

// Draws the full menu with the current item highlighted
void drawMenu() {
  const char* items[] = {
    "Night Mode", "Temp Unit", "Military Time",
    "Brightness", "Auto Dim", "Reset WiFi", "Close"
  };
  const int itemCount = 7;
  const int visible   = 5;

  tft->fillRoundRect(20, 40, 440, 240, 8, 0x1082);
  tft->drawRoundRect(20, 40, 440, 240, 8, TFT_CYAN);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(TFT_CYAN);
  tft->setCursor(190, 65);
  tft->print("SETTINGS");
  tft->drawFastHLine(30, 75, 420, TFT_CYAN);

  for (int i = 0; i < visible; i++) {
    int itemIdx = i + menuOffset;
    if (itemIdx >= itemCount) break;
    int itemY = 105 + (i * 35);

    bool isBrightnessAdjusting = (itemIdx == 3 && brightnessSelected);

    if (itemIdx == menuIndex) {
      uint16_t hlColor = isBrightnessAdjusting ? TFT_YELLOW : TFT_CYAN;
      tft->fillRoundRect(30, itemY - 16, 410, 26, 4, hlColor);
      tft->setTextColor(TFT_BLACK);
    } else {
      tft->setTextColor(TFT_WHITE);
    }

    tft->setCursor(50, itemY);
    tft->print(items[itemIdx]);

    tft->setCursor(340, itemY);
    if (itemIdx == 0) tft->print(isNightMode    ? "ON"  : "OFF");
    if (itemIdx == 1) tft->print(isCelsius      ? "C"   : "F");
    if (itemIdx == 2) tft->print(isMilitaryTime ? "ON"  : "OFF");
    if (itemIdx == 3) {
      int pct = map(BRIGHTNESS_STEPS[brightness], 191, 0, 25, 100);
      tft->printf("%d%%", pct);
    }
    if (itemIdx == 4) tft->print(isAutoDim ? "ON" : "OFF");
  }

  drawScrollbar();

  // hint text when in brightness adjust mode
  if (brightnessSelected) {
    tft->setFont(NULL);
    tft->setTextColor(TFT_YELLOW);
    tft->setCursor(100, 277);
    tft->print("Tap to adjust, hold to confirm");
  }
}

// =============================================================================
// SYSTEM / HARDWARE
// =============================================================================

// Single place that applies brightness to the backlight.
// In auto dim mode it maps g_lux to PWM. In manual mode it uses the saved step.
// NOTE: When PFET is wired, uncomment the applyBrightness() calls in loop() and selectMenuItem()
void applyBrightness() {
  if (isAutoDim && g_lux > 0) {
    int autoPwm = map(constrain((int)g_lux, 0, LUX_MAX), 0, LUX_MAX, AUTO_DIM_MIN, AUTO_DIM_MAX);
    ledcWrite(TFT_PWM, autoPwm);
  } else {
    ledcWrite(TFT_PWM, BRIGHTNESS_STEPS[brightness]);
  }
}

// Loads all settings from NVS — called once at startup before anything draws
void loadSettings() {
  prefs.begin("settings", true); // read only
  isNightMode    = prefs.getBool("nightMode",    false);
  isCelsius      = prefs.getBool("celsius",      false);
  isMilitaryTime = prefs.getBool("militaryTime", false);
  isAutoDim      = prefs.getBool("autoDim",      false);
  brightness     = prefs.getInt ("brightness",   3);
  prefs.end();
  Serial.println("[NVS] Settings loaded");
}

// Saves all settings to NVS — called whenever a setting changes
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

// Detects short vs long press and routes to the right handler
void checkUserButton() {
  static bool lastButtonState     = HIGH;
  static unsigned long pressStart = 0;
  bool currentButtonState = digitalRead(RESET_PIN);

  if (lastButtonState == HIGH && currentButtonState == LOW) {
    pressStart = millis(); // button down — start the clock
  }

  if (lastButtonState == LOW && currentButtonState == HIGH) {
    unsigned long holdTime = millis() - pressStart;
    if (holdTime < 500) onShortPress(); // quick tap
    else                onLongPress();  // held
  }

  lastButtonState = currentButtonState;
}

// Short press — opens menu, navigates items, or cancels a reset confirmation
void onShortPress() {
  if (awaitingResetConfirm) {
    awaitingResetConfirm = false; // tap cancels the reset
    drawMenu();
    return;
  }
  if (!menuOpen) openMenu();
  else           navigateMenu();
}

// Long press — selects a menu item or confirms a WiFi reset
void onLongPress() {
  if (awaitingResetConfirm) {
    awaitingResetConfirm = false;
    prefs.begin("wifi-gate", false); prefs.clear(); prefs.end();
    WiFiManager wm; wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(2000);
    ESP.restart();
  }
  if (menuOpen) selectMenuItem();
}

void checkWiFiHealth() {
  static unsigned long disconnectTime     = 0;
  static unsigned long lastRetry          = 0;
  static unsigned long lastSuccessfulSync = millis(); // start at now so first reconnect behaves correctly

  if (WiFi.status() != WL_CONNECTED) {
    if (disconnectTime == 0) disconnectTime = millis();

    if (millis() - lastRetry > 60000) { // try to reconnect every 60s
      lastRetry = millis();
      prefs.begin("wifi-gate", false);
      String vSSID = prefs.getString("ssid", "");
      String vPASS = prefs.getString("pass", "");
      prefs.end();
      if (vSSID != "") WiFi.begin(vSSID.c_str(), vPASS.c_str());
    }

    if (millis() - disconnectTime > 21600000) ESP.restart(); // 6hrs no connection — restart

  } else {
    if (disconnectTime != 0) {
      // just reconnected — only resync if it's been more than 60s since last sync
      // stops it hammering NTP every time the signal blips in and out
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
    if (!syncedThisMinute && WiFi.status() == WL_CONNECTED) { // fires once at 2:01 AM
      if (syncLocationAndTime()) lastWeatherTime = 0;
      syncedThisMinute = true;
    }
  } else {
    syncedThisMinute = false; // reset so it works again next year
  }
}

void handleDeepReset() {
  pinMode(RESET_PIN, INPUT_PULLUP);
  delay(100);
  if (digitalRead(RESET_PIN) == LOW) {
    unsigned long startHold = millis();
    tft->fillScreen(TFT_MAROON);
    tft->setFont(&FreeSans9pt7b);
    tft->setTextColor(TFT_WHITE);
    tft->setCursor(80, 140);
    tft->print("KEEP HOLDING TO RESET");

    while (digitalRead(RESET_PIN) == LOW) {
      int barWidth  = map(millis() - startHold, 0, 5000, 0, 240);
      int remaining = 5 - (int)((millis() - startHold) / 1000);

      tft->fillRect(60, 160, barWidth, 10, TFT_YELLOW);
      tft->fillRect(210, 180, 60, 40, TFT_MAROON);
      tft->setCursor(220, 210);
      tft->setFont(&FreeSansBold18pt7b);
      tft->setTextColor(TFT_WHITE);
      tft->printf("%d", remaining);

      if (millis() - startHold > 5000) { // held long enough — wipe and restart clean
        prefs.begin("wifi-gate", false); prefs.clear(); prefs.end();
        prefs.begin("settings",  false); prefs.clear(); prefs.end(); // wipe settings too
        WiFiManager wm; wm.resetSettings();
        WiFi.disconnect(true, true);
        delay(2000);
        ESP.restart();
      }
      delay(100);
    }

    // released before 5s — nothing happens
    tft->fillScreen(TFT_BLACK);
    tft->setFont(&FreeSans9pt7b);
    tft->setTextColor(TFT_GREEN);
    tft->setCursor(150, 160);
    tft->print("Reset cancelled.");
    delay(1000);
    showSplashScreen();
  }
}

// =============================================================================
// WIFI MANAGEMENT
// =============================================================================

// Checks for saved credentials — connects if found, launches setup portal if not
void manageWiFiVault() {
  prefs.begin("wifi-gate", false);
  String vSSID = prefs.getString("ssid", "");
  String vPASS = prefs.getString("pass", "");
  prefs.end();

  if (vSSID == "") launchSetupPortal();
  else             connectToWiFi(vSSID, vPASS);
}

// Shows the connect screen and waits for WiFi — runs the KITT scanner while it waits
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

  WiFi.begin(vSSID.c_str(), vPASS.c_str());

  int scannerX  = 0;
  int direction = 6;
  unsigned long lastRetry = millis();

  while (WiFi.status() != WL_CONNECTED) {
    scannerX += direction;
    if (scannerX >= 310) direction = -6; // force direction — stops it running off the edge
    if (scannerX <= 0)   direction =  6;
    runKITTScanner(scannerX);

    if (millis() - lastRetry > 20000) {
      WiFi.begin(vSSID.c_str(), vPASS.c_str());
      lastRetry = millis();
      tft->fillRect(15, 268, 450, 30, TFT_BLACK);
      tft->setFont(NULL);
      tft->setTextColor(TFT_YELLOW);
      tft->setCursor(20, 270);
      tft->print("Taking long? Hold button while plugging in to reset WiFi.");
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

// No saved credentials — launches a captive portal so the user can enter WiFi details
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
// DATA LAYER — fetches data and writes to shared globals, never touches the screen
// =============================================================================

bool fetchWeather() {
  HTTPClient http;
  http.setTimeout(5000);        // 5s response timeout
  http.setConnectTimeout(3000); // 3s connection timeout
  String url = "http://api.openweathermap.org/data/2.5/forecast?q=" + currentCity
               + "&appid=" + String(weatherKey) + "&units=imperial";
  bool success = false;
  if (http.begin(url)) {
    if (http.GET() == 200) {
      JsonDocument doc;
      deserializeJson(doc, http.getStream()); // stream directly — saves RAM
      g_currentTemp = doc["list"][0]["main"]["temp"];
      g_skyStatus   = doc["list"][0]["weather"][0]["main"].as<String>();
      g_tempHigh = -100.0;
      g_tempLow  =  200.0;
      for (int i = 0; i < FORECAST_PERIODS_24H; i++) { // find true high/low over 24hrs
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

bool syncLocationAndTime() { // loop() checks WiFi before calling this
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

// Reads TSL2591, SCD40, and SGP41 — writes to g_ globals, never touches the display
void updateLocalSensors() {
  // TSL2591 lux
  uint32_t lum  = tsl.getFullLuminosity();
  uint16_t ir   = lum >> 16;
  uint16_t full = lum & 0xFFFF;
  if (full == 0 && ir == 0) {
    g_lux = 0; // too dark to measure — skip the calculation entirely
  } else {
    float lux = tsl.calculateLux(full, ir);
    g_lux = (isnan(lux) || lux < 0 || lux > 88000) ? 0 : lux; // reject invalid results
  }

  // SCD40 — CO2, temperature, humidity
  // getDataReadyStatus only returns true when a new measurement is available
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

  // SGP41 — VOC index and NOx index
  // feed SCD40 temp and humidity into compensation algorithm for accurate readings
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
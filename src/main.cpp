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
//  Display      : ILI9341 2.8" TFT (320x240)
//  Version      : v1.6
//  Author       : Logan Calloway
//  License      : Copyright (c) 2025 Logan Calloway. All Rights Reserved.
//                 Unauthorized copying, distribution, or modification of
//                 this file, via any medium, is strictly prohibited.
// -----------------------------------------------------------------------------
//  Description  :
//    A desktop engineering dashboard built on the XIAO ESP32-C6. Pulls time
//    from NTP, weather from OpenWeatherMap, and reads live environmental data
//    from the TSL2591 (light) and BME280 (temp, humidity, pressure). Has a
//    scrollable settings menu navigated by a single hardware button. All
//    settings persist across reboots via NVS. Auto Dim drives the backlight
//    from the TSL2591 lux reading once the PFET is wired up.
//    Air quality (SGP30) will be added when a genuine sensor arrives.
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
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_BME280.h>
#include <Adafruit_TSL2591.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>

// =============================================================================
// CONFIGURATION
// =============================================================================
#define OS_VERSION    "CALLOWAY_OS v1.6"
const char* weatherKey  = "ba635bfab9190ef5a8f86cab9aaf1d0d";
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
#define RESET_PIN D6  // settings button

// =============================================================================
// LAYOUT CONSTANTS — change these to move things around on screen
// =============================================================================
#define WEATHER_X       165
#define WEATHER_CITY_Y   20
#define WEATHER_TEMP_Y   45
#define WEATHER_HILO_Y   62
#define WEATHER_LOW_X   235
#define CLOCK_X          50
#define CLOCK_Y          75
#define DATE_X           65
#define DATE_Y          145
#define DIVIDER_Y       165
#define WIFI_ICON_X     295
#define WIFI_ICON_Y      15

// Light sensor bar
#define LUX_BAR_X       10
#define LUX_BAR_Y      170
#define LUX_BAR_WIDTH   12
#define LUX_BAR_HEIGHT  60
#define LUX_MAX        400  // tweak to match your room — 400 works well indoors

// Local sensor readings (top left)
#define LOCAL_X        5
#define LOCAL_TEMP_Y   20
#define LOCAL_HUM_Y    35
#define LOCAL_PRES_Y   50

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
const long sensorInterval   =   2000;   // 2s    — local sensors
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
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
Adafruit_BME280  bme;
Adafruit_TSL2591 tsl = Adafruit_TSL2591(2591);
Preferences      prefs;
GFXcanvas16      clockCanvas(260, 45); // clock buffer — prevents flicker on second updates
GFXcanvas16      localCanvas(150, 55); // local sensor buffer — prevents flicker on 2s updates
GFXcanvas16      dateCanvas(240, 20);  // date buffer — prevents flicker on minute updates

// =============================================================================
// STATE
// =============================================================================
int  lastMin = -1, lastSec = -1;  // only redraw when something actually changed
bool isNightMode          = false;
bool hasWeatherData       = false; // don't draw weather until we have real data
bool hasBMEData           = false; // don't draw local sensors until first read
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
float g_lux       = 0;
float g_localTemp = 0;
float g_humidity  = 0;
float g_pressure  = 0;

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
};

ColorPalette getColorPalette() {
  if (isNightMode) {
    return { 0xF800, 0x8000, 0xA000, 0x4000, 0x8000, 0xFBE0, 0xF800, 0x0410,
             0x8000, 0x2104, 0x0400, 0x4000 };
  } else {
    return {
      ILI9341_WHITE, ILI9341_LIGHTGREY, ILI9341_YELLOW,
      ILI9341_DARKGREY, ILI9341_LIGHTGREY, ILI9341_ORANGE,
      ILI9341_RED, ILI9341_CYAN,
      ILI9341_YELLOW, 0x2104, ILI9341_GREEN, ILI9341_RED
    };
  }
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

  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(ILI9341_BLACK);

  showSplashScreen();

  // I2C bus — shared by TSL2591 and BME280
  Wire.begin(TFT_SDA, TFT_SCL);

  tsl.begin();
  tsl.setGain(TSL2591_GAIN_LOW);
  tsl.setTiming(TSL2591_INTEGRATIONTIME_100MS);

  if (!bme.begin(0x76, &Wire)) Serial.println("[BME] Init failed");

  handleDeepReset();
  manageWiFiVault();     // blocks here until connected — watchdog not armed yet
  syncLocationAndTime();

  tft.fillScreen(ILI9341_BLACK);

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

      dateCanvas.fillScreen(ILI9341_BLACK);
      dateCanvas.setFont(&FreeSans9pt7b);
      dateCanvas.setTextColor(col.date);
      dateCanvas.setCursor(DATE_X - 40, 15); // offset — canvas starts at x=40 on screen
      dateCanvas.print(dateBuf);

      tft.drawRGBBitmap(40, 130, dateCanvas.getBuffer(), 240, 20); // push in one shot
      tft.drawFastHLine(40, DIVIDER_Y, 240, col.line);
      lastMin = timeinfo.tm_min;
    }

    clockCanvas.fillScreen(ILI9341_BLACK); // wipe canvas before drawing new time
    clockCanvas.setFont(&FreeSansBold18pt7b);
    clockCanvas.setTextColor(col.clock);
    clockCanvas.setCursor(5, 32);

    if (isMilitaryTime) {
      // 24hr — no AM/PM
      clockCanvas.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      // 12hr — convert hour and draw AM/PM after
      int hour12 = timeinfo.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      clockCanvas.printf("%d:%02d:%02d", hour12, timeinfo.tm_min, timeinfo.tm_sec);
      int endX = clockCanvas.getCursorX();
      clockCanvas.setFont(&FreeSans9pt7b);
      clockCanvas.setTextColor(col.ampm);
      clockCanvas.setCursor(endX + 6, 32);
      clockCanvas.print((timeinfo.tm_hour >= 12) ? "PM" : "AM");
    }

    tft.drawRGBBitmap(CLOCK_X, CLOCK_Y, clockCanvas.getBuffer(), 260, 45); // push to screen in one shot
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

  tft.fillRect(160, 5, 155, 75, ILI9341_BLACK);
  tft.setFont(&FreeMono9pt7b);

  tft.setTextColor(col.city);
  tft.setCursor(WEATHER_X, WEATHER_CITY_Y);
  tft.print(currentCity.substring(0, currentCity.indexOf(','))); // drop the country code

  tft.setTextColor(col.temp);
  tft.setCursor(WEATHER_X, WEATHER_TEMP_Y);
  tft.printf("%.0f%s %s", displayTemp, unit, g_skyStatus.c_str());

  tft.setTextColor(col.high);
  tft.setCursor(WEATHER_X, WEATHER_HILO_Y);
  tft.printf("H:%.0f", displayHigh);

  tft.setTextColor(col.low);
  tft.setCursor(WEATHER_LOW_X, WEATHER_HILO_Y);
  tft.printf("L:%.0f", displayLow);
}

// Draws lux bar and BME280 local readings — waits for real data before drawing
void updateSensorDisplay() {
  ColorPalette col = getColorPalette();

  // lux bar — always draws
  int fillHeight = map(constrain((int)g_lux, 0, LUX_MAX), 0, LUX_MAX, 0, LUX_BAR_HEIGHT);
  tft.fillRect(LUX_BAR_X, LUX_BAR_Y, LUX_BAR_WIDTH, LUX_BAR_HEIGHT, col.luxBg);
  tft.fillRect(LUX_BAR_X, LUX_BAR_Y + (LUX_BAR_HEIGHT - fillHeight), LUX_BAR_WIDTH, fillHeight, col.luxBar);

  // BME280 — top left, draws as soon as first read succeeds
  if (!hasBMEData) return;

  float displayLocalTemp = isCelsius ? (g_localTemp - 32) * 5.0 / 9.0 : g_localTemp;
  const char* unit = isCelsius ? "C" : "F";

  localCanvas.fillScreen(ILI9341_BLACK);
  localCanvas.setFont(&FreeMono9pt7b);

  localCanvas.setTextColor(col.temp);
  localCanvas.setCursor(LOCAL_X, LOCAL_TEMP_Y);
  localCanvas.printf("%.1f%s", displayLocalTemp, unit);

  localCanvas.setTextColor(col.date);
  localCanvas.setCursor(LOCAL_X, LOCAL_HUM_Y);
  localCanvas.printf("HUM: %.0f%%", g_humidity);

  localCanvas.setCursor(LOCAL_X, LOCAL_PRES_Y);
  localCanvas.printf("%.0f hPa", g_pressure);

  tft.drawRGBBitmap(0, 5, localCanvas.getBuffer(), 150, 55); // push to screen in one shot
}

void drawWiFiIcon(int x, int y) {
  ColorPalette col  = getColorPalette();
  bool connected    = (WiFi.status() == WL_CONNECTED);
  int  bars         = !connected ? 0 : (WiFi.RSSI() > -60) ? 4 : (WiFi.RSSI() > -80) ? 2 : 1;
  uint16_t active   = connected ? col.wifiActive : col.wifiInactive;
  for (int i = 0; i < 4; i++) {
    tft.fillRect(x + (i * 5), y + (10 - (i * 3)), 3, 4 + (i * 3),
                 (i < bars) ? active : col.luxBg);
  }
}

void drawDynamicBorder(float temp) { // border color reflects outside temperature
  uint16_t borderColor;
  if      (temp >= 85) borderColor = ILI9341_RED;
  else if (temp >= 70) borderColor = ILI9341_ORANGE;
  else if (temp >= 55) borderColor = ILI9341_GREEN;
  else if (temp >= 40) borderColor = 0x07FF; // cyan
  else                 borderColor = ILI9341_BLUE;

  tft.drawRect(0, 0, 320, 240, borderColor);
  tft.drawRect(1, 1, 318, 238, borderColor); // double up for a bolder look
}

void runKITTScanner(int x) { // animated loading bar during WiFi connect
  int y     = 230, h = 4;
  int tailX = max(0, x - 15); // clamp so tail never draws off the left edge
  tft.fillRect(0,     y, 320, h, ILI9341_BLACK);
  tft.fillRect(tailX, y,  40, h, 0x8000);      // dark red tail
  tft.fillRect(x,     y,  10, h, ILI9341_RED);  // bright red core
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
  tft.fillScreen(ILI9341_BLACK);
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
  if (menuIndex >= menuOffset + 4) menuOffset++; // scroll down when cursor passes the window
  if (menuIndex == 0) menuOffset = 0;             // wrap back to top — reset offset too
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
      tft.fillScreen(ILI9341_BLACK);
      tft.drawRoundRect(20, 60, 280, 120, 8, ILI9341_RED);
      tft.setFont(&FreeSans9pt7b);
      tft.setTextColor(ILI9341_WHITE);
      tft.setCursor(50, 100);
      tft.print("Reset WiFi credentials?");
      tft.setTextColor(ILI9341_LIGHTGREY);
      tft.setCursor(70, 125);
      tft.print("Hold to confirm.");
      tft.setCursor(70, 148);
      tft.print("Tap to cancel.");
      break;

    case 6: closeMenu(); break;
  }
}

// Draws the scrollbar — thumb moves down as you scroll through items
void drawScrollbar() {
  const int itemCount = 7;
  const int visible   = 4;
  const int trackX    = 285;
  const int trackY    = 68;
  const int trackH    = 145;

  int thumbH = (visible * trackH) / itemCount; // thumb height proportional to visible ratio
  int thumbY = trackY + (menuOffset * trackH) / itemCount; // moves down as menuOffset increases

  tft.fillRect(trackX, trackY, 4, trackH, 0x2104);        // dim grey track
  tft.fillRect(trackX, thumbY, 4, thumbH, ILI9341_CYAN);  // cyan thumb
}

// Draws the full menu with the current item highlighted
void drawMenu() {
  const char* items[] = {
    "Night Mode", "Temp Unit", "Military Time",
    "Brightness", "Auto Dim", "Reset WiFi", "Close"
  };
  const int itemCount = 7;
  const int visible   = 4;

  tft.fillRoundRect(20, 30, 280, 190, 8, 0x1082); // dark background
  tft.drawRoundRect(20, 30, 280, 190, 8, ILI9341_CYAN);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(110, 55);
  tft.print("SETTINGS");
  tft.drawFastHLine(30, 62, 260, ILI9341_CYAN);

  for (int i = 0; i < visible; i++) {
    int itemIdx = i + menuOffset;
    if (itemIdx >= itemCount) break;
    int itemY = 90 + (i * 35);

    bool isBrightnessAdjusting = (itemIdx == 3 && brightnessSelected);

    if (itemIdx == menuIndex) {
      uint16_t hlColor = isBrightnessAdjusting ? ILI9341_YELLOW : ILI9341_CYAN;
      tft.fillRoundRect(30, itemY - 16, 250, 26, 4, hlColor); // selected highlight
      tft.setTextColor(ILI9341_BLACK);
    } else {
      tft.setTextColor(ILI9341_WHITE);
    }

    tft.setCursor(45, itemY);
    tft.print(items[itemIdx]);

    // show current value on the right for toggleable items
    tft.setCursor(205, itemY);
    if (itemIdx == 0) tft.print(isNightMode    ? "ON"  : "OFF");
    if (itemIdx == 1) tft.print(isCelsius      ? "C"   : "F");
    if (itemIdx == 2) tft.print(isMilitaryTime ? "ON"  : "OFF");
    if (itemIdx == 3) {
      int pct = map(BRIGHTNESS_STEPS[brightness], 191, 0, 25, 100);
      tft.printf("%d%%", pct);
    }
    if (itemIdx == 4) tft.print(isAutoDim ? "ON" : "OFF");
  }

  drawScrollbar();

  // hint text when in brightness adjust mode
  if (brightnessSelected) {
    tft.setFont(NULL);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(55, 217);
    tft.print("Tap to adjust, hold to confirm");
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
    tft.fillScreen(ILI9341_MAROON);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(40, 100);
    tft.print("KEEP HOLDING TO RESET");

    while (digitalRead(RESET_PIN) == LOW) {
      int barWidth  = map(millis() - startHold, 0, 5000, 0, 240);
      int remaining = 5 - (int)((millis() - startHold) / 1000);

      tft.fillRect(40,  120, barWidth, 10, ILI9341_YELLOW); // progress bar
      tft.fillRect(130, 135, 60,       40, ILI9341_MAROON); // clear old digit
      tft.setCursor(140, 160);
      tft.setFont(&FreeSansBold18pt7b);
      tft.setTextColor(ILI9341_WHITE);
      tft.printf("%d", remaining);

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
    tft.fillScreen(ILI9341_BLACK);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(60, 120);
    tft.print("Reset cancelled.");
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
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&FreeMono9pt7b);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(10, 25);
  tft.print(OS_VERSION);

  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(20, 170);
  tft.print("> ESTABLISHING NEURAL LINK");

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(20, 195);
  tft.print("  TARGET: ");
  tft.setTextColor(ILI9341_MAGENTA);
  tft.print(vSSID);

  WiFi.begin(vSSID.c_str(), vPASS.c_str());

  int scannerX  = 0;
  int direction = 6;
  unsigned long lastRetry = millis();

  while (WiFi.status() != WL_CONNECTED) {
    scannerX += direction;
    if (scannerX >= 310) direction = -6; // force direction — stops it running off the edge
    if (scannerX <= 0)   direction =  6;
    runKITTScanner(scannerX);

    if (millis() - lastRetry > 20000) { // re-kick the connection every 20s
      WiFi.begin(vSSID.c_str(), vPASS.c_str());
      lastRetry = millis();
      tft.fillRect(15, 205, 290, 30, ILI9341_BLACK);
      tft.setFont(NULL);
      tft.setTextColor(ILI9341_YELLOW);
      tft.setCursor(20, 208);
      tft.print("Taking long? Hold button while");
      tft.setCursor(20, 218);
      tft.print("plugging in to reset WiFi.");
    }
    delay(20);
  }

  tft.fillRect(15, 205, 290, 30, ILI9341_BLACK);
  tft.fillRect(0,  230, 320,  4, ILI9341_GREEN);
  tft.fillRect(15, 160, 290, 60, ILI9341_BLACK);
  tft.setTextColor(ILI9341_GREEN);
  tft.setCursor(20, 190);
  tft.print("> NEURAL LINK ESTABLISHED");

  delay(1000);
  tft.fillScreen(ILI9341_BLACK);
  tft.drawFastHLine(40, DIVIDER_Y, 240, ILI9341_DARKGREY);
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

// Reads TSL2591 and BME280 — writes to g_ globals, never touches the display
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

  // BME280 — temp stored as Fahrenheit, converted at draw time if needed
  g_localTemp = bme.readTemperature() * 9.0 / 5.0 + 32.0;
  g_humidity  = bme.readHumidity();
  g_pressure  = bme.readPressure() / 100.0F; // hPa
  hasBMEData  = true;

  Serial.printf("[LUX] %.1f  [TEMP] %.1fF  [HUM] %.1f%%  [PRES] %.0f hPa\n",
                g_lux, g_localTemp, g_humidity, g_pressure);
}

// =============================================================================
// SPLASH / SETUP SCREENS
// =============================================================================

void showSplashScreen() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&FreeMono9pt7b);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(10, 25);
  tft.print(OS_VERSION);
  tft.setFont(&FreeSansBold18pt7b);
  tft.setTextColor(ILI9341_ORANGE);
  tft.setCursor(55, 100);
  tft.print("OMNI-CORE");
}

void showSetupScreen(String apName) {
  tft.fillScreen(0x0010);
  tft.drawRoundRect(10, 10, 300, 220, 10, ILI9341_CYAN);
  tft.setFont(&FreeSansBold18pt7b);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(45, 60);
  tft.print("SETUP MODE");
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(30, 110);
  tft.print("1. Connect Phone to WiFi:");
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(60, 140);
  tft.print(apName);
}
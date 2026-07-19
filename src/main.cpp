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
#define TFT_DARKBG      0x2104  // shared dark neutral background — used in both day and night palettes

// Night Mode red-shift palette — preserves night vision, avoids blue/white
// light. Named because each shade is reused across several palette fields
// below; a couple of genuinely one-off accent colors (e.g. the amber "temp"
// tone, the teal "low" temp tone) are left as inline hex since they're not
// part of this reused red scale.
#define NIGHT_RED_BRIGHT 0xF800  // full-intensity red — most prominent elements
#define NIGHT_RED_MED     0x8000  // medium red — secondary elements
#define NIGHT_RED_DIM      0x4000  // dim red — least prominent elements
#define NIGHT_AMBER_WARN 0x8400  // warning/elevated-state accent

// =============================================================================
// CONFIGURATION
// =============================================================================
#define OS_VERSION       "CALLOWAY_OS v2.0"
#define WIFI_SETUP_AP_NAME "OmniCore-Setup"
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

// Overall air-quality status — sits in the open strip between the divider
// and the PM row. X starts at 30 like the PM/AQ rows, clearing the lux bar
// (which only occupies x:15-29) further down at the same Y range.
#define STATUS_ROW_Y   228   // screen Y where the status canvas is pushed
#define STATUS_DOT_X    30   // canvas-local
#define STATUS_DOT_Y    12   // canvas-local
#define STATUS_DOT_R      6
#define STATUS_TEXT_X    44  // canvas-local

// Menu geometry
#define MENU_X          20
#define MENU_Y          40
#define MENU_W         440
#define MENU_H         240
#define MENU_ITEM_COUNT   7  // Night Mode, Temp Unit, Military Time, Brightness, Auto Dim, Reset WiFi, Close
#define MENU_VISIBLE      5  // rows shown at once; scroll arrows page through the rest
#define MENU_ITEM_H     35
#define MENU_FIRST_Y   105  // Y baseline of first menu item
#define MENU_CLOSE_X   430  // X close button X position
#define MENU_CLOSE_Y    55  // X close button Y position

// Close button tap target — drawn as bare "X" text (no button rect), so this
// hit box is hand-tuned around the glyph rather than derived from a draw call.
#define MENU_CLOSE_HIT_X1 415
#define MENU_CLOSE_HIT_X2 455
#define MENU_CLOSE_HIT_Y1  42
#define MENU_CLOSE_HIT_Y2  72

// Scroll arrow buttons — same rect used to draw (drawMenu) and hit-test
// (handleMenuTouch), so the tap target can never drift out of sync with
// what's actually drawn on screen.
#define MENU_SCROLL_UP_X    335
#define MENU_SCROLL_DOWN_X  380
#define MENU_SCROLL_BTN_W    35
#define MENU_SCROLL_Y       253
#define MENU_SCROLL_H        25

// Reset-confirm YES/NO buttons — same rect used to draw (drawResetConfirm)
// and hit-test (handleResetConfirmTouch), so they can't drift apart.
#define RESET_YES_X   60
#define RESET_NO_X   270
#define RESET_BTN_Y  180
#define RESET_BTN_W  150
#define RESET_BTN_H   40

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
const long historyInterval  =  60000;   // 60s   — history sample for web graphs

unsigned long lastClockTime    = 0;
unsigned long lastWifiIconTime = 0;
unsigned long lastSensorTime   = 0;
unsigned long lastWeatherTime  = 0;
unsigned long lastNtpTime      = 0;
unsigned long lastHistoryTime  = 0;

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
GFXcanvas16 statusCanvas(440, 24);  // overall air-quality status dot + label

// =============================================================================
// SETTINGS — persisted to NVS. loadSettings()/saveSettings() are the only
// functions that touch flash for these; everything else just reads/writes
// the struct directly.
// =============================================================================
struct Settings {
  bool isNightMode    = false;
  bool isCelsius      = false;
  bool isMilitaryTime = false;
  bool isAutoDim      = false;
  int  brightness     = 3;  // index into BRIGHTNESS_STEPS
};

Settings settings;

// =============================================================================
// UI STATE — resets every boot, never touches flash.
// =============================================================================
int  lastMin = -1, lastSec = -1;    // dirty bits — only redraw when changed
bool hasWeatherData       = false;  // gates weather display until first fetch
bool menuOpen             = false;
bool brightnessSliderOpen = false;
bool awaitingResetConfirm = false;
int  menuOffset           = 0;      // first visible menu item index

// Touch state — start position only, last position unreliable on FT6236 lift
struct TouchState {
  bool          active    = false;  // currently mid-touch
  int           startX    = 0;
  int           startY    = 0;
  unsigned long startTime = 0;
  unsigned long lastTap   = 0;  // debounce: ignore taps under 400ms apart
  unsigned long lastPoll  = 0;  // throttles I2C polling to 50Hz
};

TouchState touchState;

// Weather globals — written by fetchWeather(), read by updateWeatherDisplay()
float  g_currentTemp = 0;
float  g_tempHigh    = 0;
float  g_tempLow     = 0;
String g_skyStatus   = "";

// Sensor readings — written by updateLocalSensors(), read by updateSensorDisplay()
// and sampleHistory(). Each sensor's own "Valid" flag gates its fields until
// the first real reading comes in. TSL2591's lux has no such gate — it always
// returns a usable value (0 in darkness/failure is itself a valid reading).
struct SensorReadings {
  float lux = 0;  // TSL2591

  bool     scd40Valid = false;  // gates co2/localTemp/humidity until first read
  uint16_t co2        = 0;      // ppm
  float    localTemp  = 0;      // stored as Fahrenheit, converted on display
  float    humidity   = 0;

  bool  sen54Valid = false;  // gates pm1/pm25/pm4/pm10/voc until first valid read
  float pm1  = 0;  // ug/m3
  float pm25 = 0;  // ug/m3
  float pm4  = 0;  // ug/m3
  float pm10 = 0;  // ug/m3
  float voc  = 0;  // index 0-500 (100 = your baseline)
};

SensorReadings sensors;

// =============================================================================
// HISTORY — in-RAM ring buffer feeding the web dashboard's 24hr graphs.
// One sample every 60s = 1440 points = 24 hours. RAM only — resets on
// reboot, which is fine since this device runs continuously. Not shown on
// the TFT; the web dashboard at omnicore.local/history reads this.
// =============================================================================
#define HISTORY_SIZE 1440   // 24hr @ 60s/sample

struct HistoryPoint {
  uint32_t t;       // seconds since boot
  uint16_t co2;
  float    temp;
  float    humidity;
  float    pm1;
  float    pm25;
  float    pm4;
  float    pm10;
  float    voc;
  float    lux;
};

HistoryPoint historyBuf[HISTORY_SIZE];
int  historyHead  = 0;      // index where the next point will be written
int  historyCount = 0;      // how many valid points exist so far (caps at HISTORY_SIZE)

// Pushes the current sensor readings into the ring buffer as one new point.
// Called on its own 60s timer from loop() — independent of the 1s sensor poll.
//
// Timestamps are real UTC epoch seconds from the NTP-synced system clock
// (set up in syncLocationAndTime() via configTime()), NOT millis()/1000.
// millis() only counts time since the ESP32 booted, so it can't be compared
// against the browser's real wall-clock time — that mismatch is what caused
// history and live points to land on completely different points on the
// dashboard's time axis.
void sampleHistory() {
  HistoryPoint &p = historyBuf[historyHead];
  time_t now = time(nullptr);
  // Before NTP has synced, time(nullptr) returns a tiny value (seconds since
  // 1970, essentially 0) — skip sampling until we have a real clock so the
  // buffer never gets seeded with bogus early timestamps.
  if (now < 1000000000) return; // ~Sept 2001 — anything before this isn't real NTP time

  p.t        = (uint32_t)now;
  p.co2      = sensors.co2;
  p.temp     = sensors.localTemp;
  p.humidity = sensors.humidity;
  p.pm1      = sensors.pm1;
  p.pm25     = sensors.pm25;
  p.pm4      = sensors.pm4;
  p.pm10     = sensors.pm10;
  p.voc      = sensors.voc;
  p.lux      = sensors.lux;

  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

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

  // Settings menu, brightness slider, and reset-confirm dialog pull colors
  // from these too, so Night Mode applies uniformly across the whole UI,
  // not just the dashboard.
  uint16_t menuPanelBg;    // menu/slider/reset dialog background
  uint16_t menuAccent;     // border, headers, divider, scroll thumb/arrows, slider fill
  uint16_t menuText;       // item labels, values, YES button text
  uint16_t menuMutedText;  // close "X", reset dialog body copy
  uint16_t menuDimText;    // item ">" arrow, slider 25%/100% labels
  uint16_t menuRowBg;      // menu item row background
  uint16_t menuControlBg;  // scrollbar track, scroll arrow buttons, reset NO button bg
  uint16_t menuSubtle;     // slider track fill, footer hint text
  uint16_t menuWarn;       // reset dialog border, YES button bg
};

ColorPalette getColorPalette() {
  if (settings.isNightMode) {
    return {
      .clock = NIGHT_RED_BRIGHT, .date = NIGHT_RED_MED, .ampm = 0xA000, .line = NIGHT_RED_DIM,
      .city = NIGHT_RED_MED, .temp = 0xFBE0, .high = NIGHT_RED_BRIGHT, .low = 0x0410,
      .luxBar = NIGHT_RED_MED, .luxBg = TFT_DARKBG, .wifiActive = 0x0400, .wifiInactive = NIGHT_RED_DIM,
      .co2Good = NIGHT_RED_DIM, .co2Warn = NIGHT_AMBER_WARN, .co2High = NIGHT_RED_MED,
      .pm25Good = NIGHT_RED_DIM, .pm25Warn = NIGHT_AMBER_WARN, .pm25High = NIGHT_RED_MED,

      .menuPanelBg = 0x2000, .menuAccent = NIGHT_RED_BRIGHT, .menuText = NIGHT_RED_BRIGHT,
      .menuMutedText = NIGHT_RED_MED, .menuDimText = NIGHT_RED_DIM, .menuRowBg = TFT_DARKBG,
      .menuControlBg = TFT_DARKBG, .menuSubtle = TFT_DARKBG, .menuWarn = NIGHT_RED_BRIGHT,
    };
  } else {
    return {
      .clock = TFT_WHITE, .date = TFT_LIGHTGREY, .ampm = TFT_YELLOW, .line = TFT_DARKGREY,
      .city = TFT_LIGHTGREY, .temp = TFT_ORANGE, .high = TFT_RED, .low = TFT_CYAN,
      .luxBar = TFT_YELLOW, .luxBg = TFT_DARKBG, .wifiActive = TFT_GREEN, .wifiInactive = TFT_RED,
      .co2Good = TFT_GREEN, .co2Warn = TFT_YELLOW, .co2High = TFT_RED,
      .pm25Good = TFT_GREEN, .pm25Warn = TFT_YELLOW, .pm25High = TFT_RED,

      .menuPanelBg = 0x1082, .menuAccent = TFT_CYAN, .menuText = TFT_WHITE,
      .menuMutedText = TFT_LIGHTGREY, .menuDimText = TFT_DARKGREY, .menuRowBg = 0x0821,
      .menuControlBg = TFT_DARKBG, .menuSubtle = 0x4208, .menuWarn = TFT_RED,
    };
  }
}

uint16_t getCO2Color(ColorPalette col) {
  if (sensors.co2 < 800)  return col.co2Good;
  if (sensors.co2 < 1200) return col.co2Warn;
  return col.co2High;
}

// PM2.5 is the health-relevant particulate number — EPA breakpoints in ug/m3
uint16_t getPM25Color(ColorPalette col) {
  if (sensors.pm25 < 12.0) return col.pm25Good;
  if (sensors.pm25 < 35.0) return col.pm25Warn;
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
void drawWiFiIcon();
void runKITTScanner(int x);
void handleRoot();
void handleData();
void handleHistory();
void startWebServer();
void sampleHistory();

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
  // First touch init attempt — this commonly fails here because the bus
  // hasn't yet been reset for the TSL2591 below; the real init happens
  // after that reset, a few lines down. Not logged since its result was
  // never meaningful on its own.
  touch.begin(40);

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

  // Re-init FT6236 after bus reset — this is the init that actually sticks,
  // since the TSL2591 above leaves the I2C bus in a state that breaks the
  // first attempt above. begin()'s return value is known unreliable on this
  // hardware (returns false even when touch works fine), so we don't branch
  // on it — just log that init was attempted and let touch itself be the
  // real verification.
  digitalWrite(CTP_RST, LOW);
  delay(10);
  digitalWrite(CTP_RST, HIGH);
  delay(100);
  touch.begin(40);
  Serial.println("[FT6206] Init attempted (result unreliable on this hardware — verify by touching screen)");

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

  // Arm watchdog after all blocking startup completes.
  //
  // Recent arduino-esp32 versions auto-initialize the Task Watchdog (TWDT)
  // during startup, before setup() ever runs (CONFIG_ESP_TASK_WDT default
  // enabled). Calling esp_task_wdt_init() on top of that without first
  // deiniting throws "TWDT already initialized" — harmless, but it means
  // our 30s/panic config never actually took effect; the framework's
  // default config was left running underneath instead. esp_task_wdt_deinit()
  // first guarantees a clean slate regardless of whether the framework beat
  // us to it, so our own config below is the one that actually applies.
  esp_task_wdt_deinit();
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
    if (!menuOpen) drawWiFiIcon();
    lastWifiIconTime = currentMillis;
  }

  if (currentMillis - lastSensorTime >= sensorInterval) {
    updateLocalSensors();
    applyBrightness();
    if (!menuOpen) updateSensorDisplay();
    lastSensorTime = currentMillis;
  }

  if (currentMillis - lastHistoryTime >= historyInterval || lastHistoryTime == 0) {
    sampleHistory();
    lastHistoryTime = currentMillis;
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
  if (now - touchState.lastPoll < 20) return;
  touchState.lastPoll = now;

  if (!touch.touched()) {
    if (touchState.active) {
      touchState.active = false;
      unsigned long holdTime = millis() - touchState.startTime;

      // process as tap if short touch and not too soon after last tap
      if (holdTime > 10 && holdTime < 500 && millis() - touchState.lastTap > 400) {
        touchState.lastTap = millis();
        if (awaitingResetConfirm) {
          handleResetConfirmTouch(touchState.startX, touchState.startY);
        } else if (brightnessSliderOpen) {
          if (touchState.startY < SLIDER_Y - 40 || touchState.startY > SLIDER_Y + 60) {
            brightnessSliderOpen = false;
            saveSettings();
            drawMenu();
          }
        } else if (menuOpen) {
          handleMenuTouch(touchState.startX, touchState.startY);
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

  if (!touchState.active) {
    touchState.active    = true;
    touchState.startX    = x;
    touchState.startY    = y;
    touchState.startTime = millis();
  }

  if (brightnessSliderOpen) {
    handleSliderTouch(x, y);
  }
}

void handleMenuTouch(int x, int y) {
  // X close button — top right corner of menu
  if (x > MENU_CLOSE_HIT_X1 && x < MENU_CLOSE_HIT_X2 &&
      y > MENU_CLOSE_HIT_Y1 && y < MENU_CLOSE_HIT_Y2) {
    closeMenu();
    return;
  }

  // tap outside menu box closes it
  if (x < MENU_X || x > MENU_X + MENU_W || y < MENU_Y || y > MENU_Y + MENU_H) {
    closeMenu();
    return;
  }

  // up arrow button — same rect drawMenu() draws it with
  if (x > MENU_SCROLL_UP_X && x < MENU_SCROLL_UP_X + MENU_SCROLL_BTN_W &&
      y > MENU_SCROLL_Y && y < MENU_SCROLL_Y + MENU_SCROLL_H) {
    if (menuOffset > 0) {
      menuOffset--;
      drawMenu();
    }
    return;
  }

  // down arrow button — same rect drawMenu() draws it with
  if (x > MENU_SCROLL_DOWN_X && x < MENU_SCROLL_DOWN_X + MENU_SCROLL_BTN_W &&
      y > MENU_SCROLL_Y && y < MENU_SCROLL_Y + MENU_SCROLL_H) {
    if (menuOffset + MENU_VISIBLE < MENU_ITEM_COUNT) {
      menuOffset++;
      drawMenu();
    }
    return;
  }

  // check which item row was tapped
  for (int i = 0; i < MENU_VISIBLE; i++) {
    int itemIdx    = i + menuOffset;
    if (itemIdx >= MENU_ITEM_COUNT) break;
    int itemTop    = MENU_FIRST_Y - 18 + (i * MENU_ITEM_H);
    int itemBottom = itemTop + MENU_ITEM_H;
    if (y >= itemTop && y <= itemBottom) {
      selectMenuItem(itemIdx);
      return;
    }
  }
}

void handleResetConfirmTouch(int x, int y) {
  // YES button — same rect drawResetConfirm() draws it with
  if (x > RESET_YES_X && x < RESET_YES_X + RESET_BTN_W &&
      y > RESET_BTN_Y && y < RESET_BTN_Y + RESET_BTN_H) {
    awaitingResetConfirm = false;
    prefs.begin("wifi-gate", false); prefs.clear(); prefs.end();
    prefs.begin("settings",  false); prefs.clear(); prefs.end();
    WiFiManager wm; wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(2000);
    ESP.restart();
  }
  // NO button — same rect drawResetConfirm() draws it with
  if (x > RESET_NO_X && x < RESET_NO_X + RESET_BTN_W &&
      y > RESET_BTN_Y && y < RESET_BTN_Y + RESET_BTN_H) {
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

  if (nearest != settings.brightness) {
    settings.brightness = nearest;
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
  drawWiFiIcon();
}

void selectMenuItem(int itemIdx) {
  switch (itemIdx) {
    case 0: // Night Mode
      settings.isNightMode = !settings.isNightMode;
      saveSettings();
      drawMenu();
      break;

    case 1: // Temp Unit
      settings.isCelsius = !settings.isCelsius;
      saveSettings();
      drawMenu();
      break;

    case 2: // Military Time
      settings.isMilitaryTime = !settings.isMilitaryTime;
      saveSettings();
      drawMenu();
      break;

    case 3: // Brightness — open slider
      brightnessSliderOpen = true;
      drawBrightnessSlider();
      break;

    case 4: // Auto Dim
      settings.isAutoDim = !settings.isAutoDim;
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
  ColorPalette col = getColorPalette();
  const int trackX = 450, trackY = 78, trackH = 190;

  int thumbH = (MENU_VISIBLE * trackH) / MENU_ITEM_COUNT;
  int thumbY = trackY + (menuOffset * trackH) / MENU_ITEM_COUNT;

  tft->fillRect(trackX, trackY, 4, trackH, col.menuControlBg);
  tft->fillRect(trackX, thumbY, 4, thumbH, col.menuAccent);
}

void drawMenu() {
  ColorPalette col = getColorPalette();
  const char* items[] = {
    "Night Mode", "Temp Unit", "Military Time",
    "Brightness", "Auto Dim", "Reset WiFi", "Close"
  };

  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, col.menuPanelBg);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, col.menuAccent);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(col.menuAccent);
  tft->setCursor(185, 65);
  tft->print("SETTINGS");
  tft->drawFastHLine(30, 75, 410, col.menuAccent);

  tft->setTextColor(col.menuMutedText);
  tft->setCursor(MENU_CLOSE_X, MENU_CLOSE_Y);
  tft->print("X");

  for (int i = 0; i < MENU_VISIBLE; i++) {
    int itemIdx = i + menuOffset;
    if (itemIdx >= MENU_ITEM_COUNT) break;
    int itemY = MENU_FIRST_Y + (i * MENU_ITEM_H);

    tft->fillRoundRect(30, itemY - 16, 410, MENU_ITEM_H - 2, 4, col.menuRowBg);
    tft->setFont(&FreeSans9pt7b);
    tft->setTextColor(col.menuText);
    tft->setCursor(50, itemY);
    tft->print(items[itemIdx]);

    tft->setCursor(340, itemY);
    if (itemIdx == 0) tft->print(settings.isNightMode    ? "ON"  : "OFF");
    if (itemIdx == 1) tft->print(settings.isCelsius      ? "C"   : "F");
    if (itemIdx == 2) tft->print(settings.isMilitaryTime ? "ON"  : "OFF");
    if (itemIdx == 3) {
      int pct = map(BRIGHTNESS_STEPS[settings.brightness], 64, 255, 25, 100);
      tft->printf("%d%%", pct);
    }
    if (itemIdx == 4) tft->print(settings.isAutoDim ? "ON" : "OFF");

    tft->setTextColor(col.menuDimText);
    tft->setCursor(420, itemY);
    tft->print(">");
  }

  drawScrollbar();

  // scroll buttons
  tft->fillRoundRect(MENU_SCROLL_UP_X, MENU_SCROLL_Y, MENU_SCROLL_BTN_W, MENU_SCROLL_H, 4, col.menuControlBg);
  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(col.menuAccent);
  tft->setCursor(344, 271);
  tft->print("^");

  tft->fillRoundRect(MENU_SCROLL_DOWN_X, MENU_SCROLL_Y, MENU_SCROLL_BTN_W, MENU_SCROLL_H, 4, col.menuControlBg);
  tft->setTextColor(col.menuAccent);
  tft->setCursor(389, 271);
  tft->print("v");

  tft->setFont(NULL);
  tft->setTextColor(col.menuSubtle);
  tft->setCursor(30, 278);
  tft->print("Tap X to close");
}

void drawBrightnessSlider() {
  ColorPalette col = getColorPalette();
  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, col.menuPanelBg);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, col.menuAccent);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(col.menuAccent);
  tft->setCursor(170, 80);
  tft->print("BRIGHTNESS");
  tft->drawFastHLine(30, 90, 410, col.menuAccent);

  int pct = map(BRIGHTNESS_STEPS[settings.brightness], 64, 255, 25, 100);
  tft->setFont(&FreeSansBold18pt7b);
  tft->setTextColor(col.menuText);
  tft->setCursor(200, 140);
  tft->printf("%d%%", pct);

  // track
  tft->fillRoundRect(SLIDER_X, SLIDER_Y, SLIDER_W, SLIDER_H, 6, col.menuSubtle);
  // fill
  int fillW = map(pct, 25, 100, 0, SLIDER_W);
  tft->fillRoundRect(SLIDER_X, SLIDER_Y, fillW, SLIDER_H, 6, col.menuAccent);
  // thumb
  int thumbX = SLIDER_X + fillW;
  tft->fillCircle(thumbX, SLIDER_Y + SLIDER_H / 2, SLIDER_THUMB_R, col.menuText);
  tft->drawCircle(thumbX, SLIDER_Y + SLIDER_H / 2, SLIDER_THUMB_R, col.menuAccent);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(col.menuDimText);
  tft->setCursor(SLIDER_X, SLIDER_Y + 35);
  tft->print("25%");
  tft->setCursor(SLIDER_X + SLIDER_W - 25, SLIDER_Y + 35);
  tft->print("100%");

  tft->setFont(NULL);
  tft->setTextColor(col.menuSubtle);
  tft->setCursor(120, 220);
  tft->print("Drag to adjust  |  Tap outside to save & close");
}

void drawResetConfirm() {
  ColorPalette col = getColorPalette();
  tft->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, col.menuPanelBg);
  tft->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, col.menuWarn);

  tft->setFont(&FreeSans9pt7b);
  tft->setTextColor(col.menuText);
  tft->setCursor(100, 100);
  tft->print("Reset WiFi credentials?");

  tft->setTextColor(col.menuMutedText);
  tft->setCursor(80, 130);
  tft->print("This will erase your saved WiFi and");
  tft->setCursor(80, 150);
  tft->print("restart the setup portal.");

  tft->fillRoundRect(RESET_YES_X, RESET_BTN_Y, RESET_BTN_W, RESET_BTN_H, 6, col.menuWarn);
  tft->setTextColor(col.menuText);
  tft->setCursor(115, 205);
  tft->print("YES");

  tft->fillRoundRect(RESET_NO_X, RESET_BTN_Y, RESET_BTN_W, RESET_BTN_H, 6, col.menuControlBg);
  tft->drawRoundRect(RESET_NO_X, RESET_BTN_Y, RESET_BTN_W, RESET_BTN_H, 6, col.menuAccent);
  tft->setTextColor(col.menuAccent);
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

    if (settings.isMilitaryTime) {
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

  float displayTemp = settings.isCelsius ? (g_currentTemp - 32) * 5.0 / 9.0 : g_currentTemp;
  float displayHigh = settings.isCelsius ? (g_tempHigh    - 32) * 5.0 / 9.0 : g_tempHigh;
  float displayLow  = settings.isCelsius ? (g_tempLow     - 32) * 5.0 / 9.0 : g_tempLow;
  const char* unit  = settings.isCelsius ? "C" : "F";

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
  int fillHeight = map(constrain((int)sensors.lux, 0, LUX_MAX), 0, LUX_MAX, 0, LUX_BAR_HEIGHT);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y, LUX_BAR_WIDTH, LUX_BAR_HEIGHT, col.luxBg);
  tft->fillRect(LUX_BAR_X, LUX_BAR_Y + (LUX_BAR_HEIGHT - fillHeight), LUX_BAR_WIDTH, fillHeight, col.luxBar);

  // SCD40 temp and humidity — top left, waits for first valid read
  if (sensors.scd40Valid) {
    float displayLocalTemp = settings.isCelsius ? (sensors.localTemp - 32) * 5.0 / 9.0 : sensors.localTemp;
    const char* unit = settings.isCelsius ? "C" : "F";

    localCanvas.fillScreen(TFT_BLACK);
    localCanvas.setFont(&FreeSans9pt7b);
    localCanvas.setTextColor(col.temp);
    localCanvas.setCursor(LOCAL_X, LOCAL_TEMP_Y);
    localCanvas.printf("%.1f%s", displayLocalTemp, unit);
    localCanvas.setTextColor(col.date);
    localCanvas.setCursor(LOCAL_X, LOCAL_HUM_Y);
    localCanvas.printf("HUM: %.0f%%", sensors.humidity);

    tft->draw16bitRGBBitmap(0, 5, localCanvas.getBuffer(), 220, 55);
  }

  // Particulate row — PM1 / PM2.5 / PM4 / PM10 from the SEN54.
  // PM2.5 is color coded since it is the health-relevant number.
  if (sensors.sen54Valid) {
    pmCanvas.fillScreen(TFT_BLACK);
    pmCanvas.setFont(&FreeSans9pt7b);

    pmCanvas.setTextColor(col.date);
    pmCanvas.setCursor(PM1_X, 17);
    pmCanvas.printf("PM1:%.1f", sensors.pm1);

    pmCanvas.setTextColor(getPM25Color(col)); // the one that matters
    pmCanvas.setCursor(PM25_X, 17);
    pmCanvas.printf("PM2.5:%.1f", sensors.pm25);

    pmCanvas.setTextColor(col.date);
    pmCanvas.setCursor(PM4_X, 17);
    pmCanvas.printf("PM4:%.1f", sensors.pm4);

    pmCanvas.setCursor(PM10_X, 17);
    pmCanvas.printf("PM10:%.1f", sensors.pm10);

    tft->draw16bitRGBBitmap(30, PM_ROW_Y, pmCanvas.getBuffer(), 440, 24);
  }

  // Air quality row — CO2 from SCD40, VOC index from SEN54.
  // Each is gated on its own sensor so one can appear before the other.
  if (sensors.scd40Valid || sensors.sen54Valid) {
    aqCanvas.fillScreen(TFT_BLACK);
    aqCanvas.setFont(&FreeSans9pt7b);

    if (sensors.scd40Valid) {
      aqCanvas.setTextColor(getCO2Color(col));
      aqCanvas.setCursor(AQ_CO2_X, 17);
      aqCanvas.printf("CO2:%dppm", sensors.co2);
    }

    if (sensors.sen54Valid) {
      aqCanvas.setTextColor(col.date);
      aqCanvas.setCursor(AQ_VOC_X, 17);
      aqCanvas.printf("VOC:%.0f", sensors.voc);
    }

    tft->draw16bitRGBBitmap(30, AQ_ROW_Y, aqCanvas.getBuffer(), 440, 24);
  }

  // Overall status — worst of CO2/PM2.5 bands, same verdict the web
  // dashboard shows. Needs both sensors online since it's a "worst of
  // both" read — showing it before either has warmed up would silently
  // claim GOOD off a default-zero PM2.5 that hasn't actually been measured.
  if (sensors.scd40Valid && sensors.sen54Valid) {
    int co2Band = sensors.co2  < 800  ? 0 : sensors.co2  < 1200 ? 1 : 2;
    int pmBand  = sensors.pm25 < 12.0 ? 0 : sensors.pm25 < 35.0 ? 1 : 2;
    int worst   = max(co2Band, pmBand);

    uint16_t statusColor = (worst == 0) ? col.co2Good : (worst == 1) ? col.co2Warn : col.co2High;
    const char* statusLabel = (worst == 0) ? "AIR QUALITY: GOOD"
                             : (worst == 1) ? "AIR QUALITY: ELEVATED"
                                            : "AIR QUALITY: POOR";

    statusCanvas.fillScreen(TFT_BLACK);
    statusCanvas.fillCircle(STATUS_DOT_X, STATUS_DOT_Y, STATUS_DOT_R, statusColor);
    statusCanvas.setFont(&FreeSans9pt7b);
    statusCanvas.setTextColor(statusColor);
    statusCanvas.setCursor(STATUS_TEXT_X, 17);
    statusCanvas.print(statusLabel);

    tft->draw16bitRGBBitmap(30, STATUS_ROW_Y, statusCanvas.getBuffer(), 440, 24);
  }
}

void drawWiFiIcon() {
  ColorPalette col = getColorPalette();
  bool connected   = (WiFi.status() == WL_CONNECTED);
  int  bars        = !connected ? 0 : (WiFi.RSSI() > -60) ? 4 : (WiFi.RSSI() > -80) ? 2 : 1;
  uint16_t active  = connected ? col.wifiActive : col.wifiInactive;
  for (int i = 0; i < 4; i++) {
    tft->fillRect(WIFI_ICON_X + (i * 6), WIFI_ICON_Y + (12 - (i * 3)), 4, 4 + (i * 3),
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
  if (settings.isAutoDim) {
    int pwm = map(constrain((int)sensors.lux, 0, LUX_MAX), 0, LUX_MAX, AUTO_DIM_MIN, AUTO_DIM_MAX);
    ledcWrite(TFT_PWM, pwm);
  } else {
    ledcWrite(TFT_PWM, BRIGHTNESS_STEPS[settings.brightness]);
  }
}

void loadSettings() {
  prefs.begin("settings", true);
  settings.isNightMode    = prefs.getBool("nightMode",    false);
  settings.isCelsius      = prefs.getBool("celsius",      false);
  settings.isMilitaryTime = prefs.getBool("militaryTime", false);
  settings.isAutoDim      = prefs.getBool("autoDim",      false);
  settings.brightness = prefs.getInt ("brightness",   3);
  prefs.end();
  Serial.println("[NVS] Settings loaded");
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putBool("nightMode",    settings.isNightMode);
  prefs.putBool("celsius",      settings.isCelsius);
  prefs.putBool("militaryTime", settings.isMilitaryTime);
  prefs.putBool("autoDim",      settings.isAutoDim);
  prefs.putInt ("brightness",   settings.brightness);
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
  tft->setFont(&FreeMono9pt7b); // match "ESTABLISHING..." above — the hint text uses a smaller font
  tft->setTextColor(TFT_GREEN);
  tft->setCursor(20, 245);
  tft->print("> NEURAL LINK ESTABLISHED");

  delay(1000);
  tft->fillScreen(TFT_BLACK);
  tft->drawFastHLine(60, DIVIDER_Y, 320, TFT_DARKGREY);
}

void launchSetupPortal() {
  showSetupScreen(WIFI_SETUP_AP_NAME);
  WiFiManager wm;
  wm.setTitle("Calloway OS Setup");

  wm.setCustomHeadElement("<style>body { background-color: #050505; color: #00FFFF; font-family: 'Courier New', monospace; margin: 0; display: flex; align-items: center; justify-content: center; min-height: 100vh; }.wrap { background: #0a0a0a; border: 2px solid #00FFFF; padding: 40px; box-shadow: 0 0 20px #00FFFF; width: 95% !important; max-width: 800px !important; }@media (min-width: 768px) {form { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: flex-end; }form div { width: 48%; }button { width: 100% !important; margin-top: 20px; }h1 { width: 100%; }}a { color: #00FFFF; text-decoration: none; font-weight: bold; padding: 12px; display: block; border-bottom: 1px solid #111; width: 100%; box-sizing: border-box; }a:hover { color: #00FF00; background: #001a1a; }img, svg, .q { filter: invert(48%) sepia(79%) saturate(2476%) hue-rotate(150deg) brightness(118%) contrast(119%); }label { color: #00FFFF; display: block; text-transform: uppercase; font-size: 0.75em; letter-spacing: 2px; margin-bottom: 5px; }input { background: #000; color: #00FF00; border: 1px solid #005555; padding: 12px; width: 100%; box-sizing: border-box; font-size: 16px; }input[type='checkbox'] { width: 18px !important; height: 18px !important; accent-color: #00FF00; display: inline-block !important; vertical-align: middle; margin: 10px 5px 10px 0 !important; }input[type='checkbox'] + label { display: inline-block !important; vertical-align: middle; width: auto !important; margin: 0 !important; font-size: 0.7em; letter-spacing: 1px; }button { background: #002222; border: 1px solid #00FFFF; color: #00FF00; padding: 15px; cursor: pointer; text-transform: uppercase; font-weight: bold; display: block; width: 100%; }button:hover { background: #00FFFF; color: #000; }h1, h2 { color: #00FF00; text-transform: uppercase; text-align: center; margin-top: 0; }</style>");

  wm.setSaveConfigCallback([]() {
    Preferences p; p.begin("wifi-gate", false);
    p.putString("ssid", WiFi.SSID());
    p.putString("pass", WiFi.psk());
    p.end();
  });

  if (!wm.startConfigPortal(WIFI_SETUP_AP_NAME)) ESP.restart();
  ESP.restart();
}

// =============================================================================
// WEB SERVER — serves a live dashboard on the local network at omnicore.local
// Reads the same sensor readings the display does. Never fetches, never draws.
// =============================================================================

#include "webpage.h" // PAGE_HTML — the dashboard's HTML/CSS/JS, kept out of this file for readability

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
    sensors.co2, sensors.pm1, sensors.pm25, sensors.pm4, sensors.pm10, sensors.voc,
    sensors.localTemp, sensors.humidity, sensors.lux,
    currentCity.substring(0, currentCity.indexOf(',')).c_str());
  server.send(200, "application/json", json);
}

// Serves the full history ring buffer as a JSON array, oldest point first.
// Buffer is a circular array — historyHead is the next-write slot, so the
// oldest valid point (when full) is exactly at historyHead. Sent in
// chronological order so the browser can plot it directly, left to right.
void handleHistory() {
  // Rough size check: up to 1440 points * ~90 bytes/point of JSON <= ~130KB.
  // WebServer streams the response, so we build directly into the response
  // rather than one giant String to avoid a huge single heap allocation.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  int startIdx = (historyCount < HISTORY_SIZE) ? 0 : historyHead;
  char pointBuf[160];

  for (int i = 0; i < historyCount; i++) {
    int idx = (startIdx + i) % HISTORY_SIZE;
    HistoryPoint &p = historyBuf[idx];

    snprintf(pointBuf, sizeof(pointBuf),
      "%s{\"t\":%lu,\"co2\":%u,\"temp\":%.1f,\"hum\":%.1f,"
      "\"pm1\":%.1f,\"pm25\":%.1f,\"pm4\":%.1f,\"pm10\":%.1f,"
      "\"voc\":%.0f,\"lux\":%.1f}",
      (i == 0) ? "" : ",",
      (unsigned long)p.t, p.co2, p.temp, p.humidity,
      p.pm1, p.pm25, p.pm4, p.pm10, p.voc, p.lux);

    server.sendContent(pointBuf);
  }

  server.sendContent("]");
}

void startWebServer() {
  if (MDNS.begin("omnicore")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[WEB] mDNS started — http://omnicore.local");
  }
  server.on("/",        handleRoot);
  server.on("/data",    handleData);
  server.on("/history", handleHistory);
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
    sensors.lux = 0;
  } else {
    float lux = tsl.calculateLux(full, ir);
    sensors.lux = (isnan(lux) || lux < 0 || lux > 88000) ? 0 : lux;
  }

  // SCD40 — only reads when sensor signals new data is ready
  uint16_t co2;
  float    temp, hum;
  bool     dataReady = false;
  scd4x.getDataReadyStatus(dataReady);
  if (dataReady) {
    uint16_t error = scd4x.readMeasurement(co2, temp, hum);
    if (!error && co2 != 0) {
      sensors.co2        = co2;
      sensors.localTemp  = temp * 9.0 / 5.0 + 32.0; // store as Fahrenheit
      sensors.humidity   = hum;
      sensors.scd40Valid = true;
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
      sensors.pm1        = senPm1;
      sensors.pm25       = senPm25;
      sensors.pm4        = senPm4;
      sensors.pm10       = senPm10;
      sensors.voc        = senVoc;
      sensors.sen54Valid = true;
    }
  }

  Serial.printf("[LUX] %.1f  [CO2] %d ppm  [TEMP] %.1fF  [HUM] %.1f%%  "
                "[PM1] %.1f  [PM2.5] %.1f  [PM4] %.1f  [PM10] %.1f  [VOC] %.0f\n",
                sensors.lux, sensors.co2, sensors.localTemp, sensors.humidity,
                sensors.pm1, sensors.pm25, sensors.pm4, sensors.pm10, sensors.voc);
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
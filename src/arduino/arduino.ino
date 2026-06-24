// Project: WorldCup2026  Version: v0.3.0
// Board: Waveshare ESP32-S3-ePaper-1.54 (200x200 B/W)
// Shows most recent FIFA World Cup 2026 match: team names, score, scorers, status.
// API: https://worldcup26.ir  (no key required)
// Libraries: GxEPD2, ArduinoJson, SensorLib, qrcode

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_adc_cal.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>   // rtc_gpio_pullup_en — hold wake pins HIGH in deep sleep
#include <driver/gpio.h>     // gpio_hold_en — latch PWR_LATCH (IO17) through deep sleep
#include <Wire.h>
#include <SensorPCF85063.hpp>
#include "qrcode.h"
#include <time.h>
#include "secrets.h"
#ifndef PRIVATE_QR_DATA
#define PRIVATE_QR_DATA "https://example.com"
#endif
#include "fifa_logo.h"

// ─── Pin definitions ──────────────────────────────────────────────────────────
#define EPD_PWR      6
#define EPD_BUSY     8
#define EPD_RST      9
#define EPD_DC      10
#define EPD_CS      11
#define EPD_SCK     12
#define EPD_MOSI    13

#define LED_PIN      3
#define PWR_BTN_PIN 18
#define PWR_LATCH   17
#define BOOT_BTN_PIN 0   // GPIO0 — boot button, active LOW
#define RTC_INT_PIN  5   // PCF85063 interrupt, active LOW, open-drain
#define RTC_SDA_PIN 47
#define RTC_SCL_PIN 48
#define BATT_ADC_CH  ADC1_CHANNEL_3   // GPIO4 — battery voltage divider

// ─── Constants ────────────────────────────────────────────────────────────────
#define SCREEN_W        200
#define SCREEN_H        200
#define API_GAMES_URL   "https://worldcup26.ir/get/games"
#define REFRESH_LIVE_US   (60ULL  * 1000000ULL)   // 60s  — during live match
#define REFRESH_IDLE_US   (300ULL * 1000000ULL)   // 5min — finished/upcoming
#define AP_SSID           "epaper-hs"
#define AP_IP             "192.168.4.1"
#define PORTAL_URL        "http://" AP_IP
#define TZ_OFFSET_S       (-3 * 3600)   // UTC-3 Buenos Aires (no DST)

// ─── RTC memory — survives deep sleep / restart, cleared on power-cycle ───────
RTC_DATA_ATTR bool firstBoot     = true;
RTC_DATA_ATTR int  lastHomeScore = -1;
RTC_DATA_ATTR int  lastAwayScore = -1;
RTC_DATA_ATTR int  lastMatchId   = -1;
RTC_DATA_ATTR bool ntpSynced     = false;
RTC_DATA_ATTR uint32_t wakeCounter = 0;   // increments each RTC-alarm wake
RTC_DATA_ATTR bool lastWasLive   = false; // last fetch saw a live match

// ─── PCF85063 RTC ─────────────────────────────────────────────────────────────
SensorPCF85063 rtc;

// ─── Display ──────────────────────────────────────────────────────────────────
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
  GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

SemaphoreHandle_t dispMutex;
#define DISP_TAKE()  xSemaphoreTake(dispMutex, portMAX_DELAY)
#define DISP_GIVE()  xSemaphoreGive(dispMutex)

// ─── Battery ──────────────────────────────────────────────────────────────────
// Voltage divider ratio on this board is 2:1 (100k + 100k).
// LiPo: 4.2V full, 3.0V empty.
int batteryPercent() {
  esp_adc_cal_characteristics_t chars;
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12, 1100, &chars);
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(BATT_ADC_CH, ADC_ATTEN_DB_12);

  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) raw += adc1_get_raw(BATT_ADC_CH);
  raw /= 8;

  uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &chars);
  float vbat = mv * 2.0f / 1000.0f;  // ×2 for the divider
  int pct = (int)((vbat - 3.0f) / (4.2f - 3.0f) * 100.0f);
  return constrain(pct, 0, 100);
}

// ─── Score alert — flash LED 3× ───────────────────────────────────────────────
void scoreBeep() {
  for (int i = 0; i < 3; i++) { ledOn(); delay(150); ledOff(); delay(150); }
}

// ─── HTTP / JSON ──────────────────────────────────────────────────────────────
bool httpGetJson(const char* url, JsonDocument& doc) {
  Serial.printf("[HTTP] GET %s\n", url);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(20000);
  int code = http.GET();
  int size = http.getSize();
  Serial.printf("[HTTP] code=%d size=%d heap=%u\n", code, size, ESP.getFreeHeap());
  if (code != 200) { http.end(); return false; }

  char* buf = (char*)malloc(size + 1);
  if (!buf) { http.end(); return false; }

  WiFiClient* stream = http.getStreamPtr();
  int got = 0;
  unsigned long t0 = millis();
  while (got < size && millis() - t0 < 20000) {
    int avail = stream->available();
    if (avail > 0) got += stream->readBytes(buf + got, min(avail, size - got));
    else delay(1);
  }
  http.end();
  buf[got] = '\0';
  Serial.printf("[HTTP] read %d/%d bytes\n", got, size);

  DeserializationError err = deserializeJson(doc, buf, got);
  free(buf);
  Serial.printf("[HTTP] parse: %s\n", err ? err.c_str() : "ok");
  return !err;
}

// ─── Text helpers ─────────────────────────────────────────────────────────────
void centreText(const char* txt, int y, const GFXfont* font) {
  display.setFont(font);
  display.setTextColor(GxEPD_BLACK);
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
  display.setCursor((SCREEN_W - tw) / 2 - x1, y);
  display.print(txt);
}

void leftText(const char* txt, int x, int y, const GFXfont* font) {
  display.setFont(font);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(x, y);
  display.print(txt);
}

void rightText(const char* txt, int x, int y, const GFXfont* font) {
  display.setFont(font);
  display.setTextColor(GxEPD_BLACK);
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
  display.setCursor(x - (int)tw - x1, y);
  display.print(txt);
}

String trunc(const String& s, int n) {
  return (s.length() > (size_t)n) ? s.substring(0, n) : s;
}

// ─── Scorer summary ───────────────────────────────────────────────────────────
String scorerName(const String& entry) {
  int space = entry.lastIndexOf(' ');
  return (space > 0) ? entry.substring(0, space) : entry;
}

String scorerSummary(const String& raw) {
  if (raw == "null" || raw.length() < 3) return "";
  String names[10];
  int count = 0, pos = 0;
  while (count < 10) {
    int open = raw.indexOf('"', pos);
    if (open < 0) break;
    int close = raw.indexOf('"', open + 1);
    if (close < 0) break;
    names[count++] = scorerName(raw.substring(open + 1, close));
    pos = close + 1;
  }
  if (!count) return "";
  String result;
  int i = 0;
  while (i < count) {
    int n = 1;
    while (i + n < count && names[i + n] == names[i]) n++;
    int sp = names[i].lastIndexOf(' ');
    String last = (sp >= 0) ? names[i].substring(sp + 1) : names[i];
    if (result.length()) result += ", ";
    result += last;
    if (n > 1) result += " x" + String(n);
    i += n;
  }
  return result;
}

// ─── PCF85063 init ────────────────────────────────────────────────────────────
bool initRTC() {
  if (!rtc.begin(Wire, RTC_SDA_PIN, RTC_SCL_PIN)) {
    Serial.println("PCF85063 not found");
    return false;
  }
  rtc.resetAlarm();   // clear the alarm flag that triggered this wakeup
  Serial.println("PCF85063 ready");
  return true;
}

// ─── NTP sync ─────────────────────────────────────────────────────────────────
// Syncs every wakeup (ESP32 system clock resets on deep-sleep boot).
// Also writes the synced time into the PCF85063 so setRTCAlarm has accurate
// current time to compute the target minute/second.
void syncNTP() {
  configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
  struct tm t;
  unsigned long t0 = millis();
  while (!getLocalTime(&t) && millis() - t0 < 5000) delay(100);
  if (getLocalTime(&t)) {
    ntpSynced = true;
    rtc.setDateTime(RTC_DateTime(t));   // keep PCF85063 in sync for alarm math
    Serial.printf("NTP: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    Serial.println("NTP sync failed");
  }
}

// ─── Set PCF85063 alarm → wakes ESP32 via IO5 EXT1 ──────────────────────────
// Uses a second-only alarm (Waveshare pattern): fires once per minute when the
// seconds register matches. Wake cadence (60s live / 5min idle) is managed by
// wakeCounter in setup — we always arm a 60s-class alarm.
void setWakeupTimer(uint32_t seconds) {
  // Disable alarm and clear flag FIRST so INT pin is released HIGH.
  rtc.disableAlarm();
  rtc.resetAlarm();
  delay(5);  // let the I2C write settle and INT pin float back HIGH

  RTC_DateTime now = rtc.getDateTime();
  if (now.getYear() < 2024) {
    // RTC not set — fall back to ESP32 internal timer
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    Serial.printf("RTC fallback: timer %us\n", seconds);
    return;
  }

  // Arm at the CURRENT second value. The PCF85063 second alarm fires once per
  // minute when SEC register == tSec. So if it's now :23, it fires at :23 next
  // minute — exactly 60s from now regardless of when in the cycle we arm it.
  uint8_t tSec = now.getSecond();

  // 1. Arm second-only alarm
  rtc.setAlarmBySecond(tSec);

  // 2. Disable weekday alarm register — SensorLib never writes reg 0x0F,
  //    so it defaults to 0x00 (weekday enabled, match Sunday). PCF85063 ANDs
  //    all enabled fields, so unless it's Sunday the alarm never fires.
  Wire.beginTransmission(0x51);
  Wire.write(0x0F);   // ALRM_WEEK_REG
  Wire.write(0x80);   // AEN_W=1 → weekday alarm disabled
  Wire.endTransmission();

  // 3. Enable alarm interrupt (AIE, CTRL2 bit 7) — drives INT LOW on match
  rtc.enableAlarm();

  // 4. Clear alarm flag (AF, CTRL2 bit 6) LAST — guarantees INT is HIGH at
  //    sleep entry so EXT1 doesn't fire immediately
  rtc.resetAlarm();
  delay(5);  // settle

  Serial.printf("PCF85063 alarm sec=%02d (now %02d:%02d:%02d +%us)\n",
                tSec, now.getHour(), now.getMinute(), now.getSecond(), seconds);
}

// All deep-sleep wake sources: RTC alarm (refresh), PWR btn (shutdown), BOOT btn (QR).
// All three lines are active-LOW and must idle HIGH during deep sleep, otherwise
// ESP_EXT1_WAKEUP_ANY_LOW sees a floating/LOW pin and wakes immediately (a tight
// ~15s wake loop). The RTC INT (IO5) is open-drain — when the alarm flag is clear
// the chip releases it high-Z, so it NEEDS an internal pull held through sleep.
// pinMode(INPUT_PULLUP) does not survive deep sleep; rtc_gpio_pullup_en does.
void goAwakeOnAllSources() {
  // All three wakeup pins are active-LOW and need internal pulls held through
  // deep sleep. pinMode(INPUT_PULLUP) does NOT survive deep sleep.
  const gpio_num_t wakePins[] = {
    (gpio_num_t)RTC_INT_PIN, (gpio_num_t)PWR_BTN_PIN, (gpio_num_t)BOOT_BTN_PIN
  };
  for (gpio_num_t p : wakePins) {
    rtc_gpio_pullup_en(p);
    rtc_gpio_pulldown_dis(p);
  }

  // Hold PWR_LATCH (IO17) HIGH through deep sleep — board stays powered on battery.
  digitalWrite(PWR_LATCH, HIGH);
  gpio_hold_en((gpio_num_t)PWR_LATCH);
  gpio_deep_sleep_hold_en();

  // RTC alarm pulls IO5 LOW → wakes via EXT1. Buttons also wake via EXT1.
  esp_sleep_enable_ext1_wakeup(
    (1ULL << RTC_INT_PIN) | (1ULL << PWR_BTN_PIN) | (1ULL << BOOT_BTN_PIN),
    ESP_EXT1_WAKEUP_ANY_LOW);
}

String currentTimeStr() {
  if (!ntpSynced) return "--:--";
  struct tm t;
  if (!getLocalTime(&t)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

// ─── Status bar ───────────────────────────────────────────────────────────────
// Built-in font (setFont nullptr): setCursor(x,y) places TOP-LEFT of glyph at (x,y).
// Glyph is 7px tall. STATUS_H=20. Center: top at (20-7)/2 = 6. Line at 20.
#define STATUS_H 20
void drawStatusBar(int battPct) {
  display.setFont(nullptr);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);

  const int textY = (STATUS_H - 7) / 2;  // = 6: top of 7px glyph centered in 20px bar
  const int iconH =  7;
  const int iconW = 11;
  const int nubW  =  2;
  const int nubH  =  3;
  const int iconY = (STATUS_H - iconH) / 2;  // = 6

  // Time on left
  String ts = currentTimeStr();
  display.setCursor(2, textY);
  display.print(ts);

  // Battery % text — right edge at SCREEN_W-2
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", battPct);
  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  int pctX = SCREEN_W - (int)tw - 2;
  display.setCursor(pctX, textY);
  display.print(buf);

  // Battery icon just left of % text
  int bx = pctX - iconW - nubW - 2;
  int by = iconY;
  display.drawRect(bx, by, iconW, iconH, GxEPD_BLACK);
  display.drawRect(bx + iconW, by + (iconH - nubH) / 2, nubW, nubH, GxEPD_BLACK);
  int fill = constrain((int)(battPct / 100.0f * (iconW - 2)), 0, iconW - 2);
  if (fill > 0) display.fillRect(bx + 1, by + 1, fill, iconH - 2, GxEPD_BLACK);

  display.drawFastHLine(0, STATUS_H, SCREEN_W, GxEPD_BLACK);
}

// ─── Private QR (secrets.h payload, never push) ───────────────────────────────
void drawPrivateQR() {
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(3)];
  qrcode_initText(&qr, qrData, 3, ECC_LOW, PRIVATE_QR_DATA);

  int scale = constrain(170 / qr.size, 1, 6);
  int qrPx  = qr.size * scale;
  int ox    = (SCREEN_W - qrPx) / 2;
  int oy    = (SCREEN_H - qrPx) / 2;

  DISP_TAKE();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (uint8_t r = 0; r < qr.size; r++)
      for (uint8_t c = 0; c < qr.size; c++)
        if (qrcode_getModule(&qr, c, r))
          display.fillRect(ox + c * scale, oy + r * scale, scale, scale, GxEPD_BLACK);
  } while (display.nextPage());
  DISP_GIVE();
}


// ─── Splash ───────────────────────────────────────────────────────────────────
void drawSplash() {
  int stride = (LOGO_W + 7) / 8;
  DISP_TAKE();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int y = 0; y < LOGO_H; y++) {
      for (int x = 0; x < LOGO_W; x++) {
        uint8_t b = pgm_read_byte(&FIFA_LOGO[y * stride + x / 8]);
        if (!((b >> (7 - (x % 8))) & 1)) display.drawPixel(x, y, GxEPD_BLACK);
      }
    }
  } while (display.nextPage());
  DISP_GIVE();
}

void drawShutdown() {
  DISP_TAKE();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    centreText("Shutting down...", SCREEN_H / 2, &FreeSans9pt7b);
  } while (display.nextPage());
  delay(1000);
  display.setFullWindow();
  display.firstPage();
  do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
  DISP_GIVE();
}

// ─── Match render ─────────────────────────────────────────────────────────────
void renderMatch(JsonObject match, int battPct) {
  String homeName  = match["home_team_name_en"] | "?";
  String awayName  = match["away_team_name_en"] | "?";
  const char* hs   = match["home_score"];
  const char* as_  = match["away_score"];
  String homeScore = hs  ? String(hs)  : "-";
  String awayScore = as_ ? String(as_) : "-";
  String group     = match["group"]    | "";
  String matchday  = match["matchday"] | "";
  String elapsed   = match["time_elapsed"] | "";

  String homeSc = "null", awaySc = "null";
  if (match["home_scorers"].is<const char*>()) homeSc = match["home_scorers"].as<String>();
  if (match["away_scorers"].is<const char*>()) awaySc = match["away_scorers"].as<String>();

  String hScorer = scorerSummary(homeSc);
  String aScorer = scorerSummary(awaySc);

  String statusLabel = (elapsed == "finished")  ? "FT" :
                       (elapsed == "live")       ? "* LIVE" :
                       (elapsed == "notstarted") ? "Upcoming" : elapsed;
  String scoreStr = homeScore + "  -  " + awayScore;
  String footer   = "Group " + group + "  MD" + matchday;

  // Check for score change and beep
  int hScore = hs  ? atoi(hs)  : -1;
  int aScore = as_ ? atoi(as_) : -1;
  int mid    = atoi(match["id"] | "0");
  bool scoreChanged = (mid == lastMatchId)
                      && (hScore != lastHomeScore || aScore != lastAwayScore)
                      && (lastHomeScore >= 0);

  if (scoreChanged) scoreBeep();

  lastMatchId   = mid;
  lastHomeScore = hScore;
  lastAwayScore = aScore;

  DISP_TAKE();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    drawStatusBar(battPct);

    // Exact font metrics (from GFXglyph yOffset/height fields):
    //   FreeSans9pt7b caps:      yOffset=-12, h=13  → top=y-12, bottom=y+1
    //   FreeMonoBold18pt7b digits: yOffset=-22, h=23 → top=y-22, bottom=y+1
    //
    // Layout (y = baseline):
    //   STATUS_H=16, separator at y=16
    //   Teams:   top must clear y=18 → baseline = 18+12 = 30, sep at 30+4 = 34
    //   Score:   top must clear y=36 → baseline = 36+22 = 58... use 82 for visual weight
    //            sep at 82+4 = 86
    //   Scorers: top at 88 → baseline = 88+12 = 100, sep at 104
    //   Remaining 96px (104-200): status at 148, footer at 178

    // FreeSans9pt7b: caps yOffset=-12, h=13 → at baseline y: top=y-12, bottom=y+1
    // FreeMonoBold18pt7b: digits yOffset=-22, h=23 → top=y-22, bottom=y+1
    // Lines drawn 4px below baseline (clear of descenders).
    // STATUS_H=20, so content starts at y=20.

    // Team names: top at 24 → baseline = 24+12 = 36, line at 40
    leftText(trunc(homeName, 10).c_str(),  6,            36, &FreeSans9pt7b);
    rightText(trunc(awayName, 10).c_str(), SCREEN_W - 6, 36, &FreeSans9pt7b);
    display.drawFastHLine(0, 40, SCREEN_W, GxEPD_BLACK);

    // Score: top at 44 → baseline = 44+22 = 66... centre at 88 looks better
    centreText(scoreStr.c_str(), 88, &FreeMonoBold18pt7b);
    display.drawFastHLine(0, 92, SCREEN_W, GxEPD_BLACK);

    // Scorers: top at 96 → baseline = 96+12 = 108, line at 112
    if (hScorer.length()) leftText(trunc(hScorer, 16).c_str(),  6,            108, &FreeSans9pt7b);
    if (aScorer.length()) rightText(trunc(aScorer, 16).c_str(), SCREEN_W - 6, 108, &FreeSans9pt7b);
    display.drawFastHLine(0, 112, SCREEN_W, GxEPD_BLACK);

    // Status + footer: 88px remaining (112–200), evenly spaced
    centreText(statusLabel.c_str(), 152, &FreeSans9pt7b);
    centreText(footer.c_str(),      182, &FreeSans9pt7b);

  } while (display.nextPage());
  DISP_GIVE();
}

// ─── Error screen ─────────────────────────────────────────────────────────────
void showError(const char* msg) {
  DISP_TAKE();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    centreText(msg, SCREEN_H / 2, &FreeSans9pt7b);
  } while (display.nextPage());
  DISP_GIVE();
}

// ─── QR code for captive portal ───────────────────────────────────────────────
// Layout (built-in 5x7 font, textSize=1, each line ~9px tall):
//   y=9:  "Connect to: epaper-hs"
//   y=18: "then scan QR"
//   QR centered between y=20 and y=185
//   y=196: "http://192.168.4.1"
void drawPortalQR() {
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(4)];
  qrcode_initText(&qr, qrData, 4, ECC_LOW, PORTAL_URL);

  int scale = constrain(160 / qr.size, 1, 6);
  int qrPx  = qr.size * scale;
  int ox    = (SCREEN_W - qrPx) / 2;
  int oy    = 22;

  DISP_TAKE();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(nullptr);
    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);

    int16_t x1, y1; uint16_t tw, th;

    const char* line1 = "Connect to WiFi: " AP_SSID;
    display.getTextBounds(line1, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor((SCREEN_W - tw) / 2, 8);
    display.print(line1);

    const char* line2 = "then scan QR code";
    display.getTextBounds(line2, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor((SCREEN_W - tw) / 2, 17);
    display.print(line2);

    for (uint8_t r = 0; r < qr.size; r++)
      for (uint8_t c = 0; c < qr.size; c++)
        if (qrcode_getModule(&qr, c, r))
          display.fillRect(ox + c * scale, oy + r * scale, scale, scale, GxEPD_BLACK);

    display.getTextBounds(PORTAL_URL, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor((SCREEN_W - tw) / 2, SCREEN_H - 3);
    display.print(PORTAL_URL);
  } while (display.nextPage());
  DISP_GIVE();
}

// ─── Captive portal (AP mode WiFi setup) ─────────────────────────────────────
// Shows QR on screen, starts AP + DNS + web server.
// User scans QR → phone opens captive portal → picks network + enters password.
// Saves to NVS then reboots.
void runCaptivePortal() {
  // Clean WiFi state before switching to AP mode
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  delay(500);

  drawPortalQR();

  DNSServer  dns;
  WebServer  server(80);
  Preferences prefs;

  dns.start(53, "*", WiFi.softAPIP());

  // Scan nearby networks for the dropdown
  int n = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < n; i++) {
    options += "<option value=\"" + WiFi.SSID(i) + "\">" +
               WiFi.SSID(i) + " (" + WiFi.RSSI(i) + " dBm)</option>\n";
  }

  String page = R"(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>WorldCup2026 WiFi Setup</title>
<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px}
input,select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box}
button{width:100%;padding:10px;background:#1a6b3a;color:#fff;border:none;border-radius:4px;font-size:16px}
</style></head><body>
<h2>&#9917; WorldCup2026 WiFi Setup</h2>
<form method='POST' action='/save'>
<label>Network</label>
<select name='ssid'>)" + options + R"(</select>
<label>Password</label>
<input type='password' name='pass' placeholder='WiFi password'>
<br><br><button type='submit'>Save &amp; Connect</button>
</form></body></html>)";

  server.on("/", HTTP_GET,  [&]() { server.send(200, "text/html", page); });
  server.on("/save", HTTP_POST, [&]() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    server.send(200, "text/html",
      "<html><body><h2>Saved! Device rebooting...</h2></body></html>");
    delay(1500);
    ESP.restart();
  });
  // Redirect all other paths to root (captive portal behaviour)
  server.onNotFound([&]() {
    server.sendHeader("Location", String("http://") + AP_IP, true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Captive portal running at " PORTAL_URL);

  while (true) {
    dns.processNextRequest();
    server.handleClient();
    delay(2);
  }
}

// ─── WiFi connect (checks NVS first, falls back to secrets.h) ─────────────────
bool connectWiFi() {
  Preferences prefs;
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", WIFI_SSID);
  String pass = prefs.getString("pass", WIFI_PASSWORD);
  prefs.end();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);   // modem sleep OK during connect — saves power
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("Connecting to %s\n", ssid.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

// ─── LED & power button ───────────────────────────────────────────────────────
void ledOn()  { digitalWrite(LED_PIN, HIGH); }
void ledOff() { digitalWrite(LED_PIN, LOW);  }

void pwrButtonTask(void*) {
  for (;;) {
    if (digitalRead(PWR_BTN_PIN) == LOW) {
      unsigned long t0 = millis();
      while (digitalRead(PWR_BTN_PIN) == LOW) {
        if (millis() - t0 > 2000) {
          for (int i = 0; i < 3; i++) { ledOff(); delay(200); ledOn(); delay(200); }
          ledOff();
          Serial.println("Power off");
          drawShutdown();
          digitalWrite(PWR_LATCH, LOW);
          delay(500);
          esp_deep_sleep_start();
        }
        delay(10);
      }
    }
    delay(20);
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  // Release the deep-sleep latch on IO17 first, then re-assert it as a normal
  // output. While held, digitalWrite() has no effect, so this order matters: we
  // must clear the hold before we can drive the latch (and later the shutdown
  // path needs to be able to pull it LOW).
  gpio_hold_dis((gpio_num_t)PWR_LATCH);
  gpio_deep_sleep_hold_dis();
  pinMode(PWR_LATCH,   OUTPUT); digitalWrite(PWR_LATCH, HIGH);  // hold power first

  Serial.begin(115200);
  pinMode(LED_PIN,     OUTPUT);
  pinMode(PWR_BTN_PIN, INPUT_PULLUP);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // ── Decide what triggered the wakeup BEFORE touching the display/RTC ──────────
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool coldBoot = (cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  uint64_t wakePin = (cause == ESP_SLEEP_WAKEUP_EXT1)
                       ? esp_sleep_get_ext1_wakeup_status() : 0;

  bool rtcWoke  = (wakePin & (1ULL << RTC_INT_PIN)) != 0;
  bool pwrWoke  = (wakePin & (1ULL << PWR_BTN_PIN)) != 0;
  bool bootWoke = (wakePin & (1ULL << BOOT_BTN_PIN)) != 0;

  // PWR button: decide long vs short press before expensive init.
  // Only act if PWR_BTN is the waker and RTC_INT is NOT — on an RTC alarm wake
  // the EXT1 status can show IO18 as LOW due to pin bounce.
  if (pwrWoke && !rtcWoke) {
    unsigned long t0 = millis();
    while (digitalRead(PWR_BTN_PIN) == LOW && millis() - t0 < 2000) delay(10);
    bool longPress = (millis() - t0 >= 2000);

    if (!longPress) {
      // Short tap → back to sleep, re-arm alarm
      setWakeupTimer(60);
      goAwakeOnAllSources();
      esp_deep_sleep_start();
    }
    // Long press → fall through to bring up display and run shutdown screen
  }

  ledOn();
  pinMode(EPD_PWR,     OUTPUT); digitalWrite(EPD_PWR, LOW);
  delay(100);

  dispMutex = xSemaphoreCreateMutex();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, coldBoot, 2, false);
  display.setRotation(0);

  // Init PCF85063 — clears the alarm flag (releases INT pin) on every wakeup
  if (!initRTC()) showError("RTC fail");

  // Long-press shutdown
  if (pwrWoke && !rtcWoke) {
    for (int i = 0; i < 3; i++) { ledOff(); delay(200); ledOn(); delay(200); }
    ledOff();
    drawShutdown();
    digitalWrite(PWR_LATCH, LOW);
    delay(500);
    esp_deep_sleep_start();
  }

  // Boot button → show private QR; wait for another press or 15s, then sleep
  if (bootWoke && !rtcWoke) {
    while (digitalRead(BOOT_BTN_PIN) == LOW) delay(10);
    delay(200);
    drawPrivateQR();
    unsigned long deadline = millis() + 15000;
    while (millis() < deadline) {
      if (digitalRead(BOOT_BTN_PIN) == LOW) {
        while (digitalRead(BOOT_BTN_PIN) == LOW) delay(10);
        break;
      }
      delay(50);
    }
    DISP_TAKE();
    display.hibernate();
    digitalWrite(EPD_PWR, HIGH);
    DISP_GIVE();
    setWakeupTimer(60);
    goAwakeOnAllSources();
    esp_deep_sleep_start();
  }

  // ── Wake throttling ──────────────────────────────────────────────────────────
  // RTC alarm wakes every 60s. During a live match refresh every wake. When idle,
  // only refresh every 5th wake (~5min) to save battery.
  if (rtcWoke && !coldBoot) {
    wakeCounter++;
    bool refreshDue = lastWasLive || (wakeCounter % 5 == 0);
    if (!refreshDue) {
      Serial.printf("Idle skip wake %u/5\n", wakeCounter % 5);
      setWakeupTimer(60);
      goAwakeOnAllSources();
      esp_deep_sleep_start();
    }
  }

  xTaskCreate(pwrButtonTask, "pwr", 2048, nullptr, 1, nullptr);

  if (firstBoot) {
    drawSplash();
    delay(2000);
    firstBoot = false;
  }
  ledOff();

  int battPct = batteryPercent();
  Serial.printf("Battery: %d%%\n", battPct);

  if (!connectWiFi()) {
    Serial.println("WiFi failed — starting captive portal");
    runCaptivePortal();  // never returns
    return;
  }
  Serial.println("WiFi: " + WiFi.localIP().toString());
  syncNTP();

  do {
    Serial.printf("Heap: %u\n", ESP.getFreeHeap());
    JsonDocument gDoc;
    if (!httpGetJson(API_GAMES_URL, gDoc)) { showError("API error"); break; }

    JsonArray games = gDoc["games"].as<JsonArray>();
    JsonObject latest;
    int latestId = -1;
    bool isLive  = false;

    for (JsonObject g : games) {
      String fin     = g["finished"] | "FALSE";
      String elapsed = g["time_elapsed"] | "";
      int    gid     = atoi(g["id"] | "0");
      bool live      = (fin == "FALSE") && (elapsed == "live");
      bool finished  = (fin == "TRUE");
      if (live     && (!isLive  || gid > latestId)) { latestId = gid; latest = g; isLive = true; }
      else if (!isLive && finished && gid > latestId) { latestId = gid; latest = g; }
    }

    if (latestId < 0) { showError("No matches yet"); break; }

    lastWasLive = isLive;   // drives wake-throttle cadence next cycle
    if (isLive) wakeCounter = 0;   // reset so idle counting starts fresh after a match

    Serial.printf("%s id=%d  %s %s-%s %s\n",
      isLive ? "LIVE" : "FT", latestId,
      (const char*)latest["home_team_name_en"],
      latest["home_score"] | "?", latest["away_score"] | "?",
      (const char*)latest["away_team_name_en"]);

    renderMatch(latest, battPct);
  } while (false);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Always re-arm the once-per-minute RTC alarm. Cadence (60s live / 5min idle)
  // is enforced by the wake-throttle skip logic above, not by the alarm period.
  setWakeupTimer(60);

  // Power down EPD — holds image with zero power
  DISP_TAKE();
  display.hibernate();
  digitalWrite(EPD_PWR, HIGH);
  DISP_GIVE();

  Serial.printf("Deep sleep — RTC alarm every 60s (live=%d, wake=%u)\n",
                lastWasLive, wakeCounter);
  Serial.flush();

  goAwakeOnAllSources();
  esp_deep_sleep_start();
}

void loop() {}

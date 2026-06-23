// Project: WorldCup2026  Version: v0.2.0
// Board: Waveshare ESP32-S3-ePaper-1.54 (200x200 B/W)
// Shows most recent FIFA World Cup 2026 match: team names, score, scorers, status.
// API: https://worldcup26.ir  (no key required)
// Libraries: GxEPD2, ArduinoJson, qrcode

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
#include "qrcode.h"
#include <time.h>
#include "secrets.h"
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
#define BUZZER_PIN   5   // wire a piezo buzzer between GPIO5 and GND
#define BATT_ADC_CH  ADC1_CHANNEL_3   // GPIO4 — battery voltage divider

// ─── Constants ────────────────────────────────────────────────────────────────
#define SCREEN_W        200
#define SCREEN_H        200
#define API_GAMES_URL   "https://worldcup26.ir/get/games"
#define REFRESH_SECONDS 60
#define AP_SSID         "WorldCup2026-Setup"
#define AP_IP           "192.168.4.1"
#define PORTAL_URL      "http://" AP_IP

// ─── RTC memory — survives deep sleep / restart, cleared on power-cycle ───────
RTC_DATA_ATTR bool  firstBoot     = true;
RTC_DATA_ATTR int   lastHomeScore = -1;
RTC_DATA_ATTR int   lastAwayScore = -1;
RTC_DATA_ATTR int   lastMatchId   = -1;
RTC_DATA_ATTR bool  ntpSynced     = false;

// ─── Display ──────────────────────────────────────────────────────────────────
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
  GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

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

// ─── Buzzer ───────────────────────────────────────────────────────────────────
void beep(int freq, int ms) {
  tone(BUZZER_PIN, freq, ms);
  delay(ms + 20);
}

void scoreBeep() {
  beep(880, 150);
  beep(1100, 150);
  beep(1320, 300);
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

// ─── NTP ──────────────────────────────────────────────────────────────────────
void syncNTP() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  struct tm t;
  unsigned long t0 = millis();
  while (!getLocalTime(&t) && millis() - t0 < 5000) delay(100);
  if (getLocalTime(&t)) {
    ntpSynced = true;
    Serial.printf("NTP: %02d:%02d UTC\n", t.tm_hour, t.tm_min);
  } else {
    Serial.println("NTP sync failed");
  }
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
// Built-in 5x7 font: glyph is 7px tall (rows 0-6 relative to top).
// GFX draws with baseline at cursor_y: top of glyph = cursor_y - 6.
// STATUS_H=16. To vertically center: cursor_y = (16 - 7) / 2 + 6 = 10.
// Battery icon h=7, center: iconY = (16 - 7) / 2 = 4.
#define STATUS_H 16
void drawStatusBar(int battPct) {
  display.setFont(nullptr);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);

  const int textY = 10;   // baseline for 5x7 font, centers glyph in 16px bar
  const int iconY =  4;   // top of 7px-tall battery icon, centered in 16px bar
  const int iconH =  7;
  const int iconW = 11;
  const int nubW  =  2;
  const int nubH  =  3;

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

  // Battery body just left of % text
  int bx = pctX - iconW - nubW - 2;
  int by = iconY;
  display.drawRect(bx, by, iconW, iconH, GxEPD_BLACK);
  // Nub on right, centered vertically on icon
  display.drawRect(bx + iconW, by + (iconH - nubH) / 2, nubW, nubH, GxEPD_BLACK);
  // Fill proportional to charge (inner area is iconW-2 wide, iconH-2 tall)
  int fill = constrain((int)(battPct / 100.0f * (iconW - 2)), 0, iconW - 2);
  if (fill > 0) display.fillRect(bx + 1, by + 1, fill, iconH - 2, GxEPD_BLACK);

  // Separator line flush below bar
  display.drawFastHLine(0, STATUS_H, SCREEN_W, GxEPD_BLACK);
}

// ─── Splash ───────────────────────────────────────────────────────────────────
void drawSplash() {
  int stride = (LOGO_W + 7) / 8;
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
}

void drawShutdown() {
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
  if (mid == lastMatchId && (hScore != lastHomeScore || aScore != lastAwayScore)
      && lastHomeScore >= 0) {
    scoreBeep();
  }
  lastMatchId   = mid;
  lastHomeScore = hScore;
  lastAwayScore = aScore;

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

    // Team names
    leftText(trunc(homeName, 10).c_str(),  6,            30, &FreeSans9pt7b);
    rightText(trunc(awayName, 10).c_str(), SCREEN_W - 6, 30, &FreeSans9pt7b);
    display.drawFastHLine(0, 34, SCREEN_W, GxEPD_BLACK);

    // Score
    centreText(scoreStr.c_str(), 82, &FreeMonoBold18pt7b);
    display.drawFastHLine(0, 86, SCREEN_W, GxEPD_BLACK);

    // Scorers
    if (hScorer.length()) leftText(trunc(hScorer, 16).c_str(),  6,            100, &FreeSans9pt7b);
    if (aScorer.length()) rightText(trunc(aScorer, 16).c_str(), SCREEN_W - 6, 100, &FreeSans9pt7b);
    display.drawFastHLine(0, 104, SCREEN_W, GxEPD_BLACK);

    // Status + footer
    centreText(statusLabel.c_str(), 148, &FreeSans9pt7b);
    centreText(footer.c_str(),      178, &FreeSans9pt7b);

  } while (display.nextPage());
}

// ─── Error screen ─────────────────────────────────────────────────────────────
void showError(const char* msg) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    centreText(msg, SCREEN_H / 2, &FreeSans9pt7b);
  } while (display.nextPage());
}

// ─── QR code for captive portal ───────────────────────────────────────────────
void drawPortalQR() {
  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(4)];
  qrcode_initText(&qr, qrData, 4, ECC_LOW, PORTAL_URL);

  int scale = constrain((SCREEN_H - 30) / qr.size, 1, 6);
  int qrPx  = qr.size * scale;
  int ox    = (SCREEN_W - qrPx) / 2;
  int oy    = 18;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(nullptr);
    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);

    const char* label = "No WiFi - scan to setup";
    int16_t x1, y1; uint16_t tw, th;
    display.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor((SCREEN_W - tw) / 2, 9);
    display.print(label);

    for (uint8_t r = 0; r < qr.size; r++)
      for (uint8_t c = 0; c < qr.size; c++)
        if (qrcode_getModule(&qr, c, r))
          display.fillRect(ox + c * scale, oy + r * scale, scale, scale, GxEPD_BLACK);

    display.getTextBounds(PORTAL_URL, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor((SCREEN_W - tw) / 2, SCREEN_H - 3);
    display.print(PORTAL_URL);
  } while (display.nextPage());
}

// ─── Captive portal (AP mode WiFi setup) ─────────────────────────────────────
// Shows QR on screen, starts AP + DNS + web server.
// User scans QR → phone opens captive portal → picks network + enters password.
// Saves to NVS then reboots.
void runCaptivePortal() {
  drawPortalQR();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  delay(500);

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
  WiFi.setSleep(false);
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
  Serial.begin(115200);

  pinMode(PWR_LATCH,   OUTPUT); digitalWrite(PWR_LATCH, HIGH);
  pinMode(LED_PIN,     OUTPUT); ledOn();
  pinMode(PWR_BTN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN,  OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(EPD_PWR,     OUTPUT); digitalWrite(EPD_PWR, LOW);
  delay(20);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);

  xTaskCreate(pwrButtonTask, "pwr", 2048, nullptr, 1, nullptr);

  if (firstBoot) {
    drawSplash();
    delay(2000);
    firstBoot = false;
  }

  int battPct = batteryPercent();
  Serial.printf("Battery: %d%%\n", battPct);

  if (!connectWiFi()) {
    Serial.println("WiFi failed — starting captive portal");
    runCaptivePortal();  // never returns
    return;
  }
  Serial.println("WiFi: " + WiFi.localIP().toString());
  syncNTP();

  {
    Serial.printf("Heap: %u\n", ESP.getFreeHeap());
    JsonDocument gDoc;
    if (!httpGetJson(API_GAMES_URL, gDoc)) { showError("API error"); goto sleep; }

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

    if (latestId < 0) { showError("No matches yet"); goto sleep; }
    Serial.printf("%s id=%d  %s %s-%s %s\n",
      isLive ? "LIVE" : "FT", latestId,
      (const char*)latest["home_team_name_en"],
      latest["home_score"] | "?", latest["away_score"] | "?",
      (const char*)latest["away_team_name_en"]);

    renderMatch(latest, battPct);
  }

sleep:
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("Sleeping %ds\n", REFRESH_SECONDS);
  delay((uint32_t)REFRESH_SECONDS * 1000);
  ESP.restart();
}

void loop() {}

// Project: WorldCup2026  Version: v0.1.0
// Board: Waveshare ESP32-S3-ePaper-1.54 (200x200 B/W)
// Shows most recent FIFA World Cup 2026 match: team names, score, scorers, status.
// API: https://worldcup26.ir  (no key required)
// Libraries: GxEPD2, ArduinoJson

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "secrets.h"
#include "fifa_logo.h"

#define EPD_PWR      6
#define EPD_BUSY     8
#define EPD_RST      9
#define EPD_DC      10
#define EPD_CS      11
#define EPD_SCK     12
#define EPD_MOSI    13

#define LED_PIN      3   // onboard LED
#define PWR_BTN_PIN 18   // power button, active LOW with internal pull-up
#define PWR_LATCH   17   // hold HIGH to keep board powered after button release

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
  GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

#define SCREEN_W        200
#define SCREEN_H        200
#define API_GAMES_URL   "https://worldcup26.ir/get/games"
#define REFRESH_SECONDS 60

// ─── HTTP / JSON ──────────────────────────────────────────────────────────────
// Stream directly from WiFiClient — no large heap allocation needed.
// stream->setTimeout ensures the socket stays open for the full 49KB body.
bool httpGetJson(const char* url, JsonDocument& doc) {
  Serial.printf("[HTTP] GET %s\n", url);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(20000);
  int code = http.GET();
  int size = http.getSize();
  Serial.printf("[HTTP] code=%d size=%d heap=%u\n", code, size, ESP.getFreeHeap());
  if (code != 200) { http.end(); return false; }

  // Read full body into heap buffer, then parse — streaming fails with low heap
  char* buf = (char*)malloc(size + 1);
  Serial.printf("[HTTP] buf=%p\n", buf);
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

// {"Lionel Messi 38'","Lionel Messi 90+5'"} → "Messi x2"
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
void renderMatch(JsonObject match) {
  String homeName  = match["home_team_name_en"] | "?";
  String awayName  = match["away_team_name_en"] | "?";
  // score can be null (not started) — default to "-"
  const char* hs = match["home_score"];
  const char* as_ = match["away_score"];
  String homeScore = hs ? String(hs) : "-";
  String awayScore = as_ ? String(as_) : "-";
  String group    = match["group"] | "";
  String matchday = match["matchday"] | "";
  String elapsed  = match["time_elapsed"] | "";

  String homeSc = "null", awaySc = "null";
  if (match["home_scorers"].is<const char*>()) homeSc = match["home_scorers"].as<String>();
  if (match["away_scorers"].is<const char*>()) awaySc = match["away_scorers"].as<String>();

  String hScorer = scorerSummary(homeSc);
  String aScorer = scorerSummary(awaySc);

  String statusLabel = (elapsed == "finished")   ? "FT" :
                       (elapsed == "live")        ? "* LIVE" :
                       (elapsed == "notstarted")  ? "Upcoming" : elapsed;
  String scoreStr = homeScore + "  -  " + awayScore;
  String footer   = "Group " + group + "  MD" + matchday;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Team names
    leftText(trunc(homeName, 10).c_str(),  6,            30, &FreeSans9pt7b);
    rightText(trunc(awayName, 10).c_str(), SCREEN_W - 6, 30, &FreeSans9pt7b);
    display.drawFastHLine(6, 36, SCREEN_W - 12, GxEPD_BLACK);

    // Score
    centreText(scoreStr.c_str(), 90, &FreeMonoBold18pt7b);
    display.drawFastHLine(6, 100, SCREEN_W - 12, GxEPD_BLACK);

    // Scorers
    if (hScorer.length()) leftText(trunc(hScorer, 16).c_str(),  6,            125, &FreeSans9pt7b);
    if (aScorer.length()) rightText(trunc(aScorer, 16).c_str(), SCREEN_W - 6, 125, &FreeSans9pt7b);
    display.drawFastHLine(6, 134, SCREEN_W - 12, GxEPD_BLACK);

    // Status + footer
    centreText(statusLabel.c_str(), 160, &FreeSans9pt7b);
    centreText(footer.c_str(),      185, &FreeSans9pt7b);

  } while (display.nextPage());
}

// ─── LED & power button ───────────────────────────────────────────────────────
void ledOn()  { digitalWrite(LED_PIN, HIGH); }
void ledOff() { digitalWrite(LED_PIN, LOW);  }

// Background task: watches for long-press on PWR button → flash LED → deep sleep
void pwrButtonTask(void*) {
  for (;;) {
    if (digitalRead(PWR_BTN_PIN) == LOW) {
      // Wait to confirm it's a long press (>2s)
      unsigned long t0 = millis();
      while (digitalRead(PWR_BTN_PIN) == LOW) {
        if (millis() - t0 > 2000) {
          // Flash LED 3x then sleep
          for (int i = 0; i < 3; i++) {
            ledOff(); delay(200);
            ledOn();  delay(200);
          }
          ledOff();
          Serial.println("Power off");
          drawShutdown();
          digitalWrite(PWR_LATCH, LOW);  // release latch — battery IC cuts power
          delay(500);
          esp_deep_sleep_start();        // fallback if latch didn't cut power
        }
        delay(10);
      }
    }
    delay(20);
  }
}

void showError(const char* msg) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    centreText(msg, SCREEN_H / 2, &FreeSans9pt7b);
  } while (display.nextPage());
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Hold power latch HIGH immediately — keeps board on after button release
  pinMode(PWR_LATCH, OUTPUT);
  digitalWrite(PWR_LATCH, HIGH);

  // LED on immediately to indicate boot
  pinMode(LED_PIN, OUTPUT);
  ledOn();

  // Power button input with pull-up
  pinMode(PWR_BTN_PIN, INPUT_PULLUP);

  // EPD3V3_EN is active-LOW — LOW = display ON
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, LOW);
  delay(20);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);

  // Start power button watcher
  xTaskCreate(pwrButtonTask, "pwr", 2048, nullptr, 1, nullptr);

  drawSplash();
  Serial.println("Splash shown");
  delay(2000);  // LED on + splash visible for 2s before proceeding

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  {
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) { showError("WiFi failed"); goto sleep; }
  Serial.println("WiFi: " + WiFi.localIP().toString());

  {
    Serial.printf("Heap: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

    JsonDocument gDoc;
    Serial.println("Fetching games...");
    if (!httpGetJson(API_GAMES_URL, gDoc)) { showError("API error"); goto sleep; }

    JsonArray games = gDoc["games"].as<JsonArray>();
    JsonObject latest;
    int latestId = -1;
    bool isLive = false;

    for (JsonObject g : games) {
      String fin     = g["finished"] | "FALSE";
      String elapsed = g["time_elapsed"] | "";
      int    gid     = atoi(g["id"] | "0");

      // "live" = in progress. "notstarted"/"finished" are excluded from live bucket.
      bool live     = (fin == "FALSE") && (elapsed == "live");
      bool finished = (fin == "TRUE");

      if (live && (!isLive || gid > latestId))        { latestId = gid; latest = g; isLive = true; }
      else if (!isLive && finished && gid > latestId) { latestId = gid; latest = g; }
    }

    if (latestId < 0) { showError("No matches yet"); goto sleep; }
    Serial.printf("%s id=%d  %s %s-%s %s\n",
      isLive ? "LIVE" : "FT", latestId,
      (const char*)latest["home_team_name_en"],
      latest["home_score"] | "?",
      latest["away_score"] | "?",
      (const char*)latest["away_team_name_en"]);

    renderMatch(latest);
  }

sleep:
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("Sleeping %ds\n", REFRESH_SECONDS);
  delay((uint32_t)REFRESH_SECONDS * 1000);
  ESP.restart();
}

void loop() {}

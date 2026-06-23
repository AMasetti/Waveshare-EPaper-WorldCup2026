// I2C scanner for Waveshare ESP32-S3-ePaper-1.54
// SDA=IO1, SCL=IO2
// Known devices: 0x51 (PCF85063 RTC), 0x70 (SHTC3)
// Touch ICs typically at: 0x14, 0x15, 0x38, 0x48

#include <Wire.h>

#define SDA_PIN 1
#define SCL_PIN 2

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);

  Serial.println("\nI2C scan — Waveshare ESP32-S3-ePaper-1.54");
  Serial.println("==========================================");

  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X", addr);
      if (addr == 0x51) Serial.print("  ← PCF85063 RTC");
      if (addr == 0x70) Serial.print("  ← SHTC3 temp/humidity");
      if (addr == 0x14 || addr == 0x15) Serial.print("  ← likely touch IC");
      if (addr == 0x38) Serial.print("  ← likely touch IC (FT6x36)");
      if (addr == 0x48) Serial.print("  ← likely touch IC");
      Serial.println();
      found++;
    }
  }

  Serial.printf("\n%d device(s) found.\n", found);
  if (found <= 2) Serial.println("No touch IC detected — this is likely the non-touch version.");
  else            Serial.println("Extra device found — may be a touch controller!");
}

void loop() {}

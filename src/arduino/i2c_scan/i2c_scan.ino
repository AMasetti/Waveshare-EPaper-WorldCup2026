// I2C scanner for Waveshare ESP32-S3-ePaper-1.54
// SDA=IO1, SCL=IO2

#include <Wire.h>

#define SDA_PIN 1
#define SCL_PIN 2

struct I2CDevice { uint8_t addr; const char* name; };

static const I2CDevice KNOWN[] = {
  // ── On this board ──────────────────────────────────────────────
  { 0x51, "PCF85063 — RTC" },
  { 0x70, "SHTC3 — Temp/Humidity" },
  // ── Touch controllers ──────────────────────────────────────────
  { 0x14, "GT911 / GT1151 — Touch" },
  { 0x15, "GT911 (alt addr) — Touch" },
  { 0x38, "FT6x36 / FT5x06 — Touch" },
  { 0x41, "STMPE811 — Touch" },
  { 0x44, "STMPE811 (alt addr) — Touch" },
  { 0x48, "ADS1x1x / TSC2007 — Touch/ADC" },
  { 0x49, "ADS1x1x (alt) / TSC2007 — Touch/ADC" },
  // ── Common sensors ─────────────────────────────────────────────
  { 0x18, "LIS3DH / MCP9808 — Accel/Temp" },
  { 0x19, "LIS3DH (alt) — Accelerometer" },
  { 0x1C, "MMA8452 — Accelerometer" },
  { 0x1D, "MMA8452 (alt) / ADXL345 — Accelerometer" },
  { 0x20, "MCP23017 / PCF8574 — IO Expander" },
  { 0x21, "MCP23017 / PCF8574 — IO Expander" },
  { 0x23, "BH1750 — Light" },
  { 0x27, "PCF8574 (alt) / LCD — IO Expander" },
  { 0x29, "VL53L0X / TCS34725 — ToF/Color" },
  { 0x36, "MAX17048 — LiPo Fuel Gauge" },
  { 0x39, "APDS-9960 / AS7341 — Gesture/Light" },
  { 0x3C, "SSD1306 — OLED 128x64" },
  { 0x3D, "SSD1306 (alt) — OLED 128x64" },
  { 0x40, "INA219 / HTU21D / SI7021 — Power/Humidity" },
  { 0x44, "SHT30 / SHT31 — Temp/Humidity" },
  { 0x45, "SHT30 (alt) — Temp/Humidity" },
  { 0x4A, "ADS1115 — ADC" },
  { 0x4B, "ADS1115 (alt) — ADC" },
  { 0x53, "ADXL345 — Accelerometer" },
  { 0x57, "MAX30102 — Heart Rate/SpO2" },
  { 0x5A, "MLX90614 / MPR121 — IR Temp/Touch" },
  { 0x60, "MCP4725 — DAC" },
  { 0x68, "MPU-6050 / DS3231 / PCF8523 — IMU/RTC" },
  { 0x69, "MPU-6050 (alt) / ICM-20948 — IMU" },
  { 0x6A, "LSM6DS3 / ICM-42688 — IMU" },
  { 0x6B, "LSM6DS3 (alt) — IMU" },
  { 0x76, "BME280 / BMP280 / MS5611 — Pressure/Temp" },
  { 0x77, "BME280 (alt) / BMP180 / BMP085 — Pressure/Temp" },
};

const char* lookupDevice(uint8_t addr) {
  for (size_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++)
    if (KNOWN[i].addr == addr) return KNOWN[i].name;
  return "Unknown device";
}

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
      Serial.printf("  0x%02X  %s\n", addr, lookupDevice(addr));
      found++;
    }
  }

  Serial.printf("\n%d device(s) found.\n", found);
}

void loop() {}

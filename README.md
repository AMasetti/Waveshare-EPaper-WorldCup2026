# WorldCup2026

FIFA World Cup 2026 live match display for the Waveshare ESP32-S3-ePaper-1.54.

Shows the most recent or currently live match: team names, score, goalscorers, and match status — refreshed every 10 minutes.

## Hardware

| | |
|---|---|
| **Board** | Waveshare ESP32-S3-ePaper-1.54 |
| **Display** | 1.54" e-Paper 200×200 B/W |
| **MCU** | ESP32-S3 (Wi-Fi + BLE) |

## Quick Start (Arduino)

### Prerequisites
- [arduino-cli](https://arduino.github.io/arduino-cli/) installed
- ESP32 Arduino core (`esp32:esp32`)
- Libraries: `GxEPD2`, `ArduinoJson` (see `src/arduino/libraries.txt`)

### Flash

1. Copy `src/arduino/secrets.h.example` to `src/arduino/secrets.h` and fill in your WiFi credentials
2. Connect the board via USB-C
3. Run:

```bash
make flash
```

### Monitor serial output

```bash
make monitor
```

Press `Ctrl-A` then `Ctrl-\` to exit.

## API

Uses [worldcup26.ir](https://worldcup26.ir) — no API key required.

## Pin Mapping

| GPIO | Function |
|------|----------|
| IO6  | EPD power enable (active LOW) |
| IO8  | EPD BUSY |
| IO9  | EPD RST |
| IO10 | EPD DC |
| IO11 | EPD CS |
| IO12 | EPD SCK |
| IO13 | EPD MOSI |
| IO3  | LED |
| IO17 | Power latch (hold HIGH to stay on) |
| IO18 | Power button (active LOW) |

## Power

- Hold power button to turn on
- Long-press (>2s) to turn off — LED flashes then board powers down

# WorldCup2026

FIFA World Cup 2026 live match display for the Waveshare ESP32-S3-ePaper-1.54.

Shows the most recent or currently live match: team names, score, goalscorers, and match status — refreshed every 10 minutes.

## Hardware

| | |
|---|---|
| **Board** | Waveshare ESP32-S3-ePaper-1.54 |
| **Display** | 1.54" e-Paper 200×200 B/W |
| **MCU** | ESP32-S3 (Wi-Fi + BLE) |

## Quick Start

### Option A — Flash a release (no source editing required)

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/)
2. Clone this repo and install dependencies once:

```bash
git clone https://github.com/AMasetti/Waveshare-EPaper-WorldCup2026.git
cd Waveshare-EPaper-WorldCup2026
make setup
```

3. Connect the board via USB-C, then flash any release with your WiFi credentials:

```bash
make flash-version VERSION=v1.0.0 SSID="MyNetwork" PASSWORD="mypass"
```

That's it — downloads the source for that version, compiles with your credentials, and flashes the board automatically.

### Option B — Build from source

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

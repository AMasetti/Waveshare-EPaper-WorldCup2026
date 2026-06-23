PROJECT      := WorldCup2026
GITHUB_REPO  := AMasetti/Waveshare-EPaper-WorldCup2026
FQBN         ?= esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc
BAUD         ?= 115200
ARDUINO_CLI  ?= arduino-cli
CIRCUITPY    ?= /Volumes/CIRCUITPY
SKETCH_DIR    = src/arduino
BUILD_DIR     = src/arduino/build
CP_DIR        = src/circuitpython
CLI_CONFIG    = src/arduino/arduino_cli_config.yaml

# Cross-platform serial port detection
PORT_GLOB_MAC   = /dev/cu.usbmodem*
PORT_GLOB_LINUX = /dev/ttyACM* /dev/ttyUSB*
FIND_PORT = $$(ls $(PORT_GLOB_MAC) 2>/dev/null | head -1 || ls $(PORT_GLOB_LINUX) 2>/dev/null | head -1)

.PHONY: help compile upload flash monitor build-clean cp-deploy cp-monitor setup flash-version

help:
	@echo ""
	@echo "$(PROJECT) — Waveshare ESP32-S3-ePaper-1.54"
	@echo ""
	@echo "First time:"
	@echo "  make setup                                    Install arduino-cli board core + libraries"
	@echo "  make flash-version VERSION=v1.0.0 \\"
	@echo "       SSID=\"MyNetwork\" PASSWORD=\"mypass\"       Download release, compile with your WiFi, flash"
	@echo ""
	@echo "Development:"
	@echo "  make compile      Compile src/arduino"
	@echo "  make upload       Upload to board"
	@echo "  make flash        compile + upload"
	@echo "  make monitor      Open serial monitor (Ctrl-A \\ to exit)"
	@echo "  make build-clean  Remove build artifacts"
	@echo "  make cp-deploy    Copy src/circuitpython/code.py to CIRCUITPY"
	@echo "  make cp-monitor   Open CircuitPython serial REPL"
	@echo ""

setup:
	@echo "Installing ESP32 core and libraries..."
	$(ARDUINO_CLI) core update-index --config-file $(CLI_CONFIG)
	$(ARDUINO_CLI) core install esp32:esp32 --config-file $(CLI_CONFIG)
	@if [ -f $(SKETCH_DIR)/libraries.txt ]; then \
		while IFS= read -r lib || [ -n "$$lib" ]; do \
			echo "$$lib" | grep -q '^#' && continue; \
			[ -z "$$lib" ] && continue; \
			$(ARDUINO_CLI) lib install "$$lib"; \
		done < $(SKETCH_DIR)/libraries.txt; \
	fi
	@echo "Done. Run: make flash-version VERSION=vX.Y.Z SSID=\"...\" PASSWORD=\"...\""

flash-version:
	@if [ -z "$(VERSION)" ]; then echo "ERROR: VERSION is required. Usage: make flash-version VERSION=v1.0.0 SSID=\"...\" PASSWORD=\"...\""; exit 1; fi
	@if [ -z "$(SSID)" ];    then echo "ERROR: SSID is required";     exit 1; fi
	@if [ -z "$(PASSWORD)" ]; then echo "ERROR: PASSWORD is required"; exit 1; fi
	@echo "Fetching source for $(VERSION) ..."
	@TMP=$$(mktemp -d) && \
	curl -fsSL "https://github.com/$(GITHUB_REPO)/archive/refs/tags/$(VERSION).tar.gz" | tar -xz -C "$$TMP" && \
	SRCDIR=$$(ls "$$TMP") && \
	echo "#define WIFI_SSID     \"$(SSID)\""  > "$$TMP/$$SRCDIR/src/arduino/secrets.h" && \
	echo "#define WIFI_PASSWORD \"$(PASSWORD)\"" >> "$$TMP/$$SRCDIR/src/arduino/secrets.h" && \
	echo "Compiling $(VERSION) with your WiFi credentials..." && \
	$(ARDUINO_CLI) compile \
	  --fqbn $(FQBN) \
	  --config-file "$$TMP/$$SRCDIR/src/arduino/arduino_cli_config.yaml" \
	  --build-path "$$TMP/build" \
	  "$$TMP/$$SRCDIR/src/arduino" && \
	PORT=$(FIND_PORT) && \
	[ -z "$$PORT" ] && echo "ERROR: No board detected. Connect via USB and try again." && rm -rf "$$TMP" && exit 1; \
	echo "Flashing to $$PORT ..." && \
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port "$$PORT" --input-dir "$$TMP/build" && \
	rm -rf "$$TMP" && \
	echo "Done! Board is running $(VERSION)."

compile:
	@echo "Compiling $(SKETCH_DIR) ..."
	$(ARDUINO_CLI) compile \
		--fqbn $(FQBN) \
		--config-file $(CLI_CONFIG) \
		--build-path $(BUILD_DIR) \
		$(SKETCH_DIR)

upload:
	@echo "Waiting for board..."
	@PORT=$(FIND_PORT); \
	[ -z "$$PORT" ] && echo "ERROR: No board detected" && exit 1; \
	echo "Uploading to $$PORT ..."; \
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $$PORT --input-dir $(BUILD_DIR)

flash: compile upload

monitor:
	@PORT=$(FIND_PORT); \
	[ -z "$$PORT" ] && echo "ERROR: No board detected" && exit 1; \
	echo "Connected to $$PORT — Ctrl-A then Ctrl-\\ to exit"; \
	screen $$PORT $(BAUD)

build-clean:
	rm -rf $(BUILD_DIR)

cp-deploy:
	@if [ ! -d "$(CIRCUITPY)" ]; then echo "ERROR: CIRCUITPY not found"; exit 1; fi
	cp $(CP_DIR)/code.py $(CIRCUITPY)/code.py
	@if [ -d "$(CP_DIR)/lib" ]; then cp -r $(CP_DIR)/lib $(CIRCUITPY)/; fi
	@echo "Deployed to $(CIRCUITPY)"

cp-monitor:
	@PORT=$(FIND_PORT); \
	[ -z "$$PORT" ] && echo "ERROR: No serial port found" && exit 1; \
	screen $$PORT $(BAUD)

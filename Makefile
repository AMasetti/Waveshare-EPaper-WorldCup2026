PROJECT      := WorldCup2026
FQBN         ?= esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc
BAUD         ?= 115200
ARDUINO_CLI  ?= arduino-cli
CIRCUITPY    ?= /Volumes/CIRCUITPY
SKETCH_DIR    = src/arduino
BUILD_DIR     = src/arduino/build
CP_DIR        = src/circuitpython
CLI_CONFIG    = src/arduino/arduino_cli_config.yaml

.PHONY: help compile upload flash monitor build-clean cp-deploy cp-monitor

help:
	@echo ""
	@echo "$(PROJECT) — Waveshare ESP32-S3-ePaper-1.54"
	@echo "  make compile      Compile src/arduino"
	@echo "  make upload       Upload to board"
	@echo "  make flash        compile + upload"
	@echo "  make monitor      Open serial monitor (Ctrl-A \\ to exit)"
	@echo "  make build-clean  Remove build artifacts"
	@echo "  make cp-deploy    Copy src/circuitpython/code.py to CIRCUITPY"
	@echo "  make cp-monitor   Open CircuitPython serial REPL"
	@echo ""

compile:
	@echo "Compiling $(SKETCH_DIR) ..."
	$(ARDUINO_CLI) compile \
		--fqbn $(FQBN) \
		--config-file $(CLI_CONFIG) \
		--build-path $(BUILD_DIR) \
		$(SKETCH_DIR)

upload:
	@echo "Waiting for board..."
	@until ls /dev/cu.usbmodem* >/dev/null 2>&1; do sleep 0.5; done
	@PORT=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); \
	echo "Uploading to $$PORT ..."; \
	$(ARDUINO_CLI) upload --fqbn $(FQBN) --port $$PORT --input-dir $(BUILD_DIR)

flash: compile upload

monitor:
	@until ls /dev/cu.usbmodem* >/dev/null 2>&1; do sleep 0.5; done
	@PORT=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); \
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
	@PORT=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); \
	[ -z "$$PORT" ] && echo "ERROR: No serial port found" && exit 1; \
	screen $$PORT $(BAUD)

# Konfiguration
BOARD_FQBN = esp32:esp32:esp32c6:UploadSpeed=921600,CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,EraseFlash=none
PORT = /dev/ttyUSB0      # Linux
# PORT = /dev/tty.usbserial-*  # macOS
# PORT = COM3            # Windows
BUILD_DIR = ./build
SKETCH = src/main.cpp

.PHONY: all compile upload monitor clean

all: compile

compile:
	arduino-cli compile --fqbn $(BOARD_FQBN) --build-path $(BUILD_DIR) .

upload:
	arduino-cli upload --fqbn $(BOARD_FQBN) --port $(PORT) --build-path $(BUILD_DIR) .

monitor:
	arduino-cli monitor --port $(PORT) --config baudrate=115200

clean:
	rm -rf $(BUILD_DIR)

# Kombinierter Befehl: bauen & flashen
flash: compile upload

# Entwicklungszyklus: bauen, flashen, monitor
dev: compile upload monitor

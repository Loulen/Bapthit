PORT ?= /dev/ttyUSB0
BAUD ?= 115200
IDF_ENV = . $(HOME)/.espressif/v6.0/esp-idf/export.sh 2>/dev/null

.PHONY: build build-dev flash flash-dev monitor flash-monitor serial-fill backend backend-install backend-stop clean

## ESP32 firmware

build:
	$(IDF_ENV) && idf.py build

# Dev build: SSID becomes BAPTHIT-DEV. Forces a reconfigure because
# BAPTHIT_DEV is read at CMake time from the environment.
build-dev:
	$(IDF_ENV) && BAPTHIT_DEV=1 idf.py reconfigure build

flash:
	$(IDF_ENV) && idf.py -p $(PORT) flash

flash-dev:
	$(IDF_ENV) && BAPTHIT_DEV=1 idf.py reconfigure -p $(PORT) flash

monitor:
	$(IDF_ENV) && idf.py -p $(PORT) monitor

flash-monitor:
	$(IDF_ENV) && idf.py -p $(PORT) flash monitor

## Testing

serial-fill:
	python3 -c "\
	import serial, time; \
	ser = serial.Serial('$(PORT)', $(BAUD), timeout=1); \
	time.sleep(0.5); \
	[( \
		ser.write(f'SCORE:{s}\n'.encode()), \
		print(f'Sent SCORE:{s}'), \
		time.sleep(0.3) \
	) for s in [750, 820, 430, 910, 670, 550, 880, 340, 790, 600]]; \
	ser.close()"

serial-score:
	@test -n "$(SCORE)" || (echo "Usage: make serial-score SCORE=750" && exit 1)
	python3 -c "\
	import serial, time; \
	ser = serial.Serial('$(PORT)', $(BAUD), timeout=1); \
	time.sleep(0.3); \
	ser.write(b'SCORE:$(SCORE)\n'); \
	print('Sent SCORE:$(SCORE)'); \
	ser.close()"

## Laptop backend

VENV = backend/.venv

$(VENV)/bin/python:
	python3 -m venv $(VENV)

backend-install: $(VENV)/bin/python
	$(VENV)/bin/pip install -r backend/requirements.txt

backend: $(VENV)/bin/python
	$(VENV)/bin/python backend/server.py

backend-stop:
	@pkill -f "backend/server.py" && echo "Backend stopped" || echo "Backend not running"

## Housekeeping

clean:
	$(IDF_ENV) && idf.py fullclean

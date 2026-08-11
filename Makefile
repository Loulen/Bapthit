PORT ?= /dev/ttyUSB0
IDF_ENV = . $(HOME)/.espressif/v6.0/esp-idf/export.sh 2>/dev/null

.PHONY: build flash monitor flash-monitor backend backend-install backend-stop clean

## ESP32 firmware

build:
	$(IDF_ENV) && idf.py build

flash:
	$(IDF_ENV) && idf.py -p $(PORT) flash

monitor:
	$(IDF_ENV) && idf.py -p $(PORT) monitor

flash-monitor:
	$(IDF_ENV) && idf.py -p $(PORT) flash monitor

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

# Merge Bapthit + PunchMeter Design

## Context

Two ESP32 projects that are two halves of the same system:
- **Bapthit** (ESP-IDF, C): Wi-Fi AP, captive portal, HTTP server, web dashboard, OTA updates. Currently uses mock score data.
- **PunchMeter** (PlatformIO/Arduino, C++): Reads motion + position sensors, calculates punch score (0-999), displays on 3-digit 7-segment display.

Goal: merge into a single firmware. Constraint: colleague actively develops PunchMeter and only knows Arduino/C++.

## Decisions

| Decision | Choice |
|----------|--------|
| Build system | ESP-IDF with Arduino as managed component |
| PunchMeter integration | Git submodule at `components/punchmeter/` |
| Standalone PunchMeter | Still works via PlatformIO, unchanged |
| Score flow | FreeRTOS queue (PunchMeter -> Bapthit) |
| Config flow | Shared mutex-protected struct (Bapthit -> PunchMeter) |
| Score output | Both 7-segment display AND web UI |
| PunchMeter runtime | Dedicated FreeRTOS task on core 1 |
| Arduino entrypoint | Disabled (`CONFIG_AUTOSTART_ARDUINO=n`) |

## PunchMeter Repo Structure (after refactor)

```
PunchMeter/
  src/
    main.cpp            <- standalone Arduino entrypoint (thin shim)
    punchmeter.cpp      <- core logic: sensors, scoring, display
    punchmeter.h        <- public API: punchmeter_setup(), punchmeter_loop(), get_last_score()
    Pins.h              <- unchanged
  idf_component/
    CMakeLists.txt      <- ESP-IDF component descriptor, sources from ../src/
  platformio.ini        <- unchanged, standalone build still works
```

`main.cpp` becomes a shim:
```cpp
#include "punchmeter.h"
void setup() { punchmeter_setup(); }
void loop()  { punchmeter_loop(); }
```

Core logic moves to `punchmeter.cpp`/`.h`. The `idf_component/CMakeLists.txt` registers only `punchmeter.cpp` and `Pins.h` (excludes `main.cpp`).

## Bapthit Repo Structure (after integration)

```
Bapthit/
  components/
    punchmeter/            <- git submodule -> github.com/Tabourette/PunchMeter
  main/
    main.c                 <- app_main(): WiFi, HTTP, DNS, PunchMeter task, glue code
    index.html
    idf_component.yml      <- declares arduino-esp32 dependency
    CMakeLists.txt
  CMakeLists.txt           <- adds EXTRA_COMPONENT_DIRS for punchmeter
  partitions.csv
  sdkconfig.defaults       <- adds CONFIG_AUTOSTART_ARDUINO=n
```

## Runtime Architecture

```
app_main()
  |-- NVS, netif, event loop init
  |-- Wi-Fi AP start ("BAPTHIT")
  |-- Create score queue (FreeRTOS)
  |-- Create config struct + mutex
  |-- Start PunchMeter task (core 1)
  |     |-- punchmeter_setup()
  |     |-- loop:
  |           |-- punchmeter_loop()  -> reads sensors, calculates score
  |           |-- display on 7-segment
  |           |-- xQueueSend(score)
  |           |-- read config via mutex
  |-- Start HTTP server (core 0)
  |     |-- GET /           -> serve index.html
  |     |-- GET /api/scores -> drain queue, return JSON
  |     |-- POST /api/ota   -> firmware update
  |     |-- POST /api/config -> lock mutex, update config
  |-- Start DNS server (core 0)
```

## Arduino as Component

Declared via `idf_component.yml`:
```yaml
dependencies:
  espressif/arduino-esp32: "^3.1"
```

Disabled auto-start in `sdkconfig.defaults`:
```
CONFIG_AUTOSTART_ARDUINO=n
```

Arduino core APIs available but `app_main()` stays in control.

## Communication

**Scores (PunchMeter -> Web):** FreeRTOS queue. PunchMeter pushes score events as they happen. HTTP handler drains and accumulates into score array.

**Config (Web -> PunchMeter):** Shared `config_t` struct protected by mutex. Web handler writes on POST /api/config. PunchMeter reads each loop iteration.

## Changes Required

### PunchMeter repo
1. Extract core logic from `main.cpp` into `punchmeter.cpp` / `punchmeter.h`
2. `main.cpp` becomes thin shim calling `punchmeter_setup()` / `punchmeter_loop()`
3. Add `idf_component/CMakeLists.txt` for ESP-IDF integration
4. Expose `get_last_score()` + accept config struct pointer

### Bapthit repo
1. Add PunchMeter as git submodule
2. Add `idf_component.yml` for arduino-esp32 dependency
3. Add glue code in `main.c`: FreeRTOS task, score queue, config struct
4. Replace mock scores with real queue-fed data
5. Update `sdkconfig.defaults` with `CONFIG_AUTOSTART_ARDUINO=n`
6. Update root `CMakeLists.txt` with `EXTRA_COMPONENT_DIRS`

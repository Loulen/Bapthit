# Merge Bapthit + PunchMeter Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Merge PunchMeter (Arduino/C++) into Bapthit (ESP-IDF) as a git submodule so both run on a single ESP32 — real sensor scores replace mock data, displayed on both 7-segment and web UI.

**Architecture:** PunchMeter is refactored into a library with a public API (`punchmeter_setup`/`punchmeter_loop`/`get_last_score`), added as a git submodule under `components/punchmeter/`. Arduino-as-component provides Arduino APIs within ESP-IDF. A FreeRTOS task on core 1 runs the PunchMeter loop; a queue passes scores to the HTTP server; a mutex-protected struct passes config back.

**Tech Stack:** ESP-IDF v6.0, arduino-esp32 ^3.1 (managed component), FreeRTOS, C/C++

---

### Task 1: Refactor PunchMeter — extract core logic into library files

**Files:**
- Create: `/home/llenoir/Documents/perso/IoT/PunchMeter/src/punchmeter.h`
- Create: `/home/llenoir/Documents/perso/IoT/PunchMeter/src/punchmeter.cpp`
- Modify: `/home/llenoir/Documents/perso/IoT/PunchMeter/src/main.cpp`

**Step 1: Create `punchmeter.h` — public API header**

```cpp
#ifndef PUNCHMETER_H
#define PUNCHMETER_H

#include <Arduino.h>

// Bag position states
enum BagPos {
    DOWN,
    INMEAS,
    UP,
    UNKNOWN
};

// Configuration that can be updated externally
struct PunchmeterConfig {
    unsigned long scoreRef;  // microseconds, default 500000
};

// Initialize pins, serial, display
void punchmeter_setup();

// One iteration of the sensor/scoring/display loop
void punchmeter_loop();

// Get the last computed score (0-999), or -1 if no score yet
int punchmeter_get_last_score();

// Update configuration (thread-safe: caller handles synchronization)
void punchmeter_set_config(const PunchmeterConfig *cfg);

#endif
```

**Step 2: Create `punchmeter.cpp` — move core logic from main.cpp**

```cpp
#include "punchmeter.h"
#include "Pins.h"
#include <math.h>

static unsigned long start_time = 0;
static unsigned long score = 0;
static unsigned long scoreRef = 500 * 1000;
static int last_score = -1;

static byte OutPins[] = {hun_B0, hun_B1, hun_B2, hun_B3,
                         ten_B0, ten_B1, ten_B2, ten_B3,
                         unit_B0, unit_B1, unit_B2, unit_B3};
static byte InPins[] = {motSensPin, posSensPin};

static void clearDisplay() {
    for (byte i : OutPins) {
        digitalWrite(i, HIGH);
    }
}

static void displayScore(unsigned long s) {
    byte digits[3];
    digits[0] = (s / 100) % 10;
    digits[1] = (s / 10) % 10;
    digits[2] = s % 10;
    for (byte i = 0; i < 3; i++) {
        byte digit = digits[i];
        for (byte j = 0; j < 4; j++) {
            byte pinIndex = i * 4 + j;
            if (digit & (1 << j)) {
                digitalWrite(OutPins[pinIndex], HIGH);
            } else {
                digitalWrite(OutPins[pinIndex], LOW);
            }
        }
    }
}

static void rollScore(unsigned long s) {
    unsigned long current = 0;
    int incr = 9;
    while (current < s) {
        current += incr;
        if (current > s) {
            current = s;
            break;
        }
        displayScore(current);
        incr = ceil(9 - 9 * (s - current) / s);
        delay(50);
    }
    displayScore(current);
}

static BagPos getBagPos() {
    if (digitalRead(posSensPin) == LOW && digitalRead(motSensPin) == LOW) {
        return DOWN;
    } else if (digitalRead(posSensPin) == HIGH && digitalRead(motSensPin) == HIGH) {
        return INMEAS;
    } else if (digitalRead(posSensPin) == HIGH && digitalRead(motSensPin) == LOW) {
        return UP;
    } else {
        return UNKNOWN;
    }
}

void punchmeter_setup() {
    Serial.begin(9600);
    for (byte i : OutPins) {
        pinMode(i, OUTPUT);
        digitalWrite(i, LOW);
    }
    for (byte i : InPins) {
        pinMode(i, INPUT);
    }
    clearDisplay();
    delay(1000);
    start_time = micros();
}

void punchmeter_loop() {
    clearDisplay();

    if (getBagPos() == DOWN) {
        displayScore(999);
        start_time = micros();
        last_score = 999;
    } else if (getBagPos() == INMEAS) {
        score = micros() - start_time;
        if (score < scoreRef) {
            displayScore(999);
            last_score = 999;
        } else {
            score = 999 - (999 * (score - scoreRef)) / (10 * scoreRef);
            displayScore(score);
            last_score = (int)score;
        }
    }
}

int punchmeter_get_last_score() {
    return last_score;
}

void punchmeter_set_config(const PunchmeterConfig *cfg) {
    scoreRef = cfg->scoreRef;
}
```

**Step 3: Reduce `main.cpp` to a thin shim**

Replace entire content of `main.cpp` with:

```cpp
#include "punchmeter.h"

void setup() {
    punchmeter_setup();
}

void loop() {
    punchmeter_loop();
}
```

**Step 4: Verify standalone PlatformIO build**

Run: `cd /home/llenoir/Documents/perso/IoT/PunchMeter && pio run`
Expected: Compilation succeeds with no errors.

**Step 5: Commit in PunchMeter repo**

```bash
cd /home/llenoir/Documents/perso/IoT/PunchMeter
git add src/punchmeter.h src/punchmeter.cpp src/main.cpp
git commit -m "refactor: extract core logic into punchmeter library

Moves sensor reading, scoring, and display logic into punchmeter.cpp/h
with a public API. main.cpp becomes a thin shim. No behavior change —
enables reuse as an ESP-IDF component."
```

**Acceptance Criteria:**

- [ ] Run `cd /home/llenoir/Documents/perso/IoT/PunchMeter && pio run`
      Expected: BUILD SUCCESS, no compilation errors
- [ ] Run `grep -c "punchmeter_setup\|punchmeter_loop" /home/llenoir/Documents/perso/IoT/PunchMeter/src/main.cpp`
      Expected: 2 (only the two calls, no logic in main)
- [ ] Run `grep "punchmeter_get_last_score" /home/llenoir/Documents/perso/IoT/PunchMeter/src/punchmeter.h`
      Expected: Function declaration is present

---

### Task 2: Add ESP-IDF component descriptor to PunchMeter

**Files:**
- Create: `/home/llenoir/Documents/perso/IoT/PunchMeter/idf_component/CMakeLists.txt`

**Step 1: Create `idf_component/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "../src/punchmeter.cpp"
    INCLUDE_DIRS "../src"
    REQUIRES arduino
)
```

This registers only `punchmeter.cpp` (not `main.cpp`) and depends on the `arduino` component for Arduino APIs.

**Step 2: Commit**

```bash
cd /home/llenoir/Documents/perso/IoT/PunchMeter
git add idf_component/CMakeLists.txt
git commit -m "feat: add ESP-IDF component descriptor

Allows PunchMeter to be used as an ESP-IDF component via
EXTRA_COMPONENT_DIRS. Only registers the library files,
not the standalone main.cpp entrypoint."
```

**Step 3: Push to remote**

```bash
cd /home/llenoir/Documents/perso/IoT/PunchMeter
git push
```

**Acceptance Criteria:**

- [ ] Run `cat /home/llenoir/Documents/perso/IoT/PunchMeter/idf_component/CMakeLists.txt`
      Expected: Contains `idf_component_register` with `../src/punchmeter.cpp`, no reference to `main.cpp`
- [ ] Run `cd /home/llenoir/Documents/perso/IoT/PunchMeter && pio run`
      Expected: BUILD SUCCESS (idf_component/ directory doesn't affect PlatformIO build)

---

### Task 3: Add PunchMeter as git submodule in Bapthit

**Files:**
- Create: `/home/llenoir/Documents/perso/IoT/Bapthit/components/punchmeter/` (submodule)

**Step 1: Initialize Bapthit as a git repo (if not already)**

```bash
cd /home/llenoir/Documents/perso/IoT/Bapthit
git init  # skip if already a git repo
```

**Step 2: Add the submodule**

```bash
cd /home/llenoir/Documents/perso/IoT/Bapthit
git submodule add https://github.com/Tabourette/PunchMeter components/punchmeter
```

**Step 3: Commit**

```bash
git add .gitmodules components/punchmeter
git commit -m "feat: add PunchMeter as git submodule

PunchMeter is included at components/punchmeter/ for ESP-IDF
component integration."
```

**Acceptance Criteria:**

- [ ] Run `ls /home/llenoir/Documents/perso/IoT/Bapthit/components/punchmeter/src/punchmeter.h`
      Expected: File exists (submodule populated)
- [ ] Run `cat /home/llenoir/Documents/perso/IoT/Bapthit/.gitmodules`
      Expected: Contains `[submodule "components/punchmeter"]` with the GitHub URL

---

### Task 4: Add arduino-esp32 managed component and update build config

**Files:**
- Create: `/home/llenoir/Documents/perso/IoT/Bapthit/main/idf_component.yml`
- Modify: `/home/llenoir/Documents/perso/IoT/Bapthit/CMakeLists.txt`
- Modify: `/home/llenoir/Documents/perso/IoT/Bapthit/sdkconfig.defaults`
- Modify: `/home/llenoir/Documents/perso/IoT/Bapthit/main/CMakeLists.txt`

**Step 1: Create `main/idf_component.yml`**

```yaml
dependencies:
  espressif/arduino-esp32: "^3.1"
```

**Step 2: Update root `CMakeLists.txt` — add PunchMeter component path**

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/punchmeter/idf_component"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bapthit)
```

**Step 3: Update `main/CMakeLists.txt` — add punchmeter dependency**

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_server esp_app_format app_update lwip punchmeter
    EMBED_TXTFILES "index.html"
)
```

Note: the component registered via `idf_component/CMakeLists.txt` in PunchMeter will be named based on the directory name. We need to verify the exact component name ESP-IDF assigns — it may be the parent directory name or the `idf_component` directory name. If ESP-IDF names it `idf_component`, we may need to adjust (see acceptance criteria).

**Step 4: Update `sdkconfig.defaults` — disable Arduino autostart**

Append to the end of the file:

```
# Arduino as component (no autostart — we manage our own tasks)
CONFIG_AUTOSTART_ARDUINO=n
```

**Step 5: Commit**

```bash
cd /home/llenoir/Documents/perso/IoT/Bapthit
git add main/idf_component.yml CMakeLists.txt main/CMakeLists.txt sdkconfig.defaults
git commit -m "feat: configure ESP-IDF build for arduino + punchmeter

- Add arduino-esp32 as managed component dependency
- Register PunchMeter component via EXTRA_COMPONENT_DIRS
- Disable Arduino autostart (Bapthit controls task creation)
- Add punchmeter to main component REQUIRES"
```

**Step 6: Test that the build resolves dependencies**

Run: `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py reconfigure`
Expected: CMake configuration succeeds, arduino-esp32 component is downloaded, punchmeter component is found.

**Acceptance Criteria:**

- [ ] Run `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py reconfigure 2>&1 | tail -20`
      Expected: Configuration completes without errors, punchmeter and arduino components listed
- [ ] Run `cat /home/llenoir/Documents/perso/IoT/Bapthit/main/idf_component.yml`
      Expected: Contains `espressif/arduino-esp32: "^3.1"`
- [ ] Run `grep "AUTOSTART_ARDUINO" /home/llenoir/Documents/perso/IoT/Bapthit/sdkconfig.defaults`
      Expected: `CONFIG_AUTOSTART_ARDUINO=n`

---

### Task 5: Write glue code — FreeRTOS task, score queue, config struct

**Files:**
- Modify: `/home/llenoir/Documents/perso/IoT/Bapthit/main/main.c`

**Step 1: Add includes and shared data structures at top of main.c**

After the existing includes, add:

```c
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "punchmeter.h"

// ---------------------------------------------------------------------------
// PunchMeter integration — score queue + config
// ---------------------------------------------------------------------------

#define SCORE_QUEUE_SIZE 16

typedef struct {
    int score;
    int64_t timestamp_ms;  // millis since boot
} score_event_t;

static QueueHandle_t score_queue = NULL;

// Shared config protected by mutex
typedef struct {
    unsigned long scoreRef;
} shared_config_t;

static shared_config_t shared_config = { .scoreRef = 500000 };
static SemaphoreHandle_t config_mutex = NULL;
```

**Step 2: Add the PunchMeter FreeRTOS task**

```c
static void punchmeter_task(void *arg)
{
    punchmeter_setup();

    int prev_score = -1;

    while (1) {
        // Check for config updates
        if (xSemaphoreTake(config_mutex, 0) == pdTRUE) {
            PunchmeterConfig cfg = { .scoreRef = shared_config.scoreRef };
            punchmeter_set_config(&cfg);
            xSemaphoreGive(config_mutex);
        }

        punchmeter_loop();

        // Push new score to queue when it changes
        int current_score = punchmeter_get_last_score();
        if (current_score >= 0 && current_score != prev_score) {
            score_event_t evt = {
                .score = current_score,
                .timestamp_ms = esp_timer_get_time() / 1000,
            };
            xQueueSend(score_queue, &evt, 0);  // non-blocking
            prev_score = current_score;
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // yield to other tasks
    }
}
```

**Step 3: Add a helper to drain queue into the score array**

```c
static void drain_score_queue(void)
{
    score_event_t evt;
    while (xQueueReceive(score_queue, &evt, 0) == pdTRUE) {
        int64_t total_minutes = evt.timestamp_ms / 60000;
        int hour = (int)(total_minutes / 60) % 24;
        int minute = (int)(total_minutes % 60);
        add_score(hour, minute, evt.score);
    }
}
```

**Step 4: Update `scores_get_handler` to drain queue before responding**

Add `drain_score_queue();` as the first line inside `scores_get_handler()`.

**Step 5: Update `app_main` — create queue, mutex, and start task**

Replace the `generate_mock_scores()` call with:

```c
    // Create PunchMeter communication primitives
    score_queue = xQueueCreate(SCORE_QUEUE_SIZE, sizeof(score_event_t));
    config_mutex = xSemaphoreCreateMutex();

    // Start PunchMeter on core 1
    xTaskCreatePinnedToCore(punchmeter_task, "punchmeter", 4096, NULL, 5, NULL, 1);
```

**Step 6: Remove `generate_mock_scores` function entirely**

Delete the `generate_mock_scores()` function definition (lines 58-68 of current main.c).

**Step 7: Commit**

```bash
cd /home/llenoir/Documents/perso/IoT/Bapthit
git add main/main.c
git commit -m "feat: integrate PunchMeter via FreeRTOS task and score queue

- PunchMeter runs on core 1 as a dedicated FreeRTOS task
- Scores flow via FreeRTOS queue to the HTTP handler
- Config (scoreRef) shared via mutex-protected struct
- Replaces mock score generation with real sensor data"
```

**Acceptance Criteria:**

- [ ] Run `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py build 2>&1 | tail -5`
      Expected: Build succeeds, binary generated
- [ ] Run `grep "generate_mock_scores" /home/llenoir/Documents/perso/IoT/Bapthit/main/main.c`
      Expected: No matches (mock data removed)
- [ ] Run `grep "punchmeter_task" /home/llenoir/Documents/perso/IoT/Bapthit/main/main.c`
      Expected: Function definition and xTaskCreatePinnedToCore call found
- [ ] Run `grep "drain_score_queue" /home/llenoir/Documents/perso/IoT/Bapthit/main/main.c`
      Expected: Called inside scores_get_handler

---

### Task 6: Add config endpoint to HTTP server

**Files:**
- Modify: `/home/llenoir/Documents/perso/IoT/Bapthit/main/main.c`

**Step 1: Add config POST handler**

```c
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Simple manual parse: {"scoreRef":600000}
    char *key = strstr(buf, "\"scoreRef\"");
    if (key) {
        char *colon = strchr(key, ':');
        if (colon) {
            unsigned long val = strtoul(colon + 1, NULL, 10);
            if (val > 0) {
                xSemaphoreTake(config_mutex, portMAX_DELAY);
                shared_config.scoreRef = val;
                xSemaphoreGive(config_mutex);
                ESP_LOGI(TAG, "Config updated: scoreRef=%lu", val);
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

**Step 2: Register the endpoint in `start_webserver()`**

Add before the captive portal catch-all:

```c
    const httpd_uri_t config_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
    };
    httpd_register_uri_handler(server, &config_uri);
```

**Step 3: Commit**

```bash
cd /home/llenoir/Documents/perso/IoT/Bapthit
git add main/main.c
git commit -m "feat: add /api/config endpoint for PunchMeter settings

POST {\"scoreRef\": 600000} to update the scoring threshold.
Value is passed to PunchMeter via mutex-protected shared config."
```

**Acceptance Criteria:**

- [ ] Run `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py build 2>&1 | tail -5`
      Expected: Build succeeds
- [ ] Run `grep "config_post_handler" /home/llenoir/Documents/perso/IoT/Bapthit/main/main.c`
      Expected: Function definition and URI registration both found

---

### Task 7: Full build and flash verification

**Files:** None (verification only)

**Step 1: Clean build**

Run: `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py fullclean && idf.py build`
Expected: Full build succeeds from scratch.

**Step 2: Check binary size fits in partition**

Run: `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py size`
Expected: App binary < 1.3 MB (factory partition is 0x140000 = 1,310,720 bytes).

**Step 3: Flash and monitor**

Run: `cd /home/llenoir/Documents/perso/IoT/Bapthit && idf.py flash monitor -p /dev/ttyUSB0`
Expected:
- Firmware boots, logs show "Bapthit firmware v... started"
- Wi-Fi AP "BAPTHIT" is visible
- "punchmeter" task starts (serial output from PunchMeter setup)
- Connecting to BAPTHIT and opening 192.168.4.1 shows the web UI
- Scores from the sensors appear in the web UI

**Step 4: Final commit (if any build fixes were needed)**

```bash
cd /home/llenoir/Documents/perso/IoT/Bapthit
git add -A
git commit -m "fix: build fixes from integration testing"
```

**Acceptance Criteria:**

- [ ] Run `idf.py build 2>&1 | grep "Project build complete"`
      Expected: "Project build complete. To flash, run: ..."
- [ ] Run `idf.py size 2>&1 | grep "Total image size"`
      Expected: Total image size < 1,310,720 bytes
- [ ] Flash to device and observe serial monitor
      Expected: Boot log shows PunchMeter task started, no crash/reboot loops
- [ ] Connect phone to "BAPTHIT" Wi-Fi, open 192.168.4.1
      Expected: Web UI loads, scores tab shows real sensor data

# Config / Time Sync / Photo Fix Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Batch three features: fix the selfie photo button, expose all 11 PunchmeterConfig fields to the app, and sync ESP32 wall-clock time from the backend at registration.

**Architecture:** Additive changes across four layers:
1. PunchMeter submodule — extend the existing `PunchmeterConfig` struct and `set_config` with 9 new fields (purely additive).
2. ESP32 firmware (`main/main.cpp`) — mirror the struct, extend `shared_config_t`, extend POST `/api/config`, add GET `/api/config`, add time sync state + helper, parse time fields in `/api/backend`, use wall clock in `drain_score_queue`.
3. FastAPI backend (`backend/server.py`) — send `hour/minute/second` in the register payload.
4. Embedded web UI (`main/index.html`) — replace the broken `<button>` + hidden input pattern with a `<label>` wrapper, add a "Config PunchMeter" form in Settings, wire GET/POST `/api/config`.

**Tech Stack:** ESP-IDF v6.0, Arduino-as-component (PunchMeter), FreeRTOS, esp_http_server, FastAPI, vanilla HTML/JS.

**Reference design:** `ai_docs/2026-04-11-features-config-time-photo-design.md`

---

## Task 1: Extend `PunchmeterConfig` struct in the submodule

**Files:**
- Modify: `components/punchmeter/src/punchmeter.h:7-10`

**Step 1: Replace the struct definition**

Current:
```cpp
struct PunchmeterConfig {
    unsigned long maxScore;  // microseconds, scores faster than this get 999 (default 20000)
    unsigned long minScore;  // microseconds, scores slower than this get 0 (default 100000)
};
```

Replace with:
```cpp
struct PunchmeterConfig {
    unsigned long maxScore;        // microseconds, scores faster than this get 999 (default 20000)
    unsigned long minScore;        // microseconds, scores slower than this get 0 (default 100000)
    int defaultRollDelay;          // ms (default 15)
    int rollDelayMod;              // multiplier (default 1)
    int rollThresh;                // (default 60)
    int slowRollThresh;            // (default 7)
    int slowRollDelayMod;          // (default 100)
    int defaultIncrement;          // (default 15)
    int blinkDelay;                // ms (default 600)
    int waveDuration;              // (default 1, declared int in cpp)
    int waveDelay;                 // ms (default 200)
};
```

**Step 2: No commit yet** — bundled with Task 2 inside the submodule.

**Acceptance Criteria:**
- [ ] Read `components/punchmeter/src/punchmeter.h` — struct has 11 fields in the listed order.

---

## Task 2: Extend `punchmeter_set_config` to copy the 9 new fields

**Files:**
- Modify: `components/punchmeter/src/punchmeter.cpp:146-151`

**Step 1: Replace the function body**

Current:
```cpp
void punchmeter_set_config(const PunchmeterConfig *cfg) {
    if (cfg) {
        maxScore = cfg->maxScore;
        minScore = cfg->minScore;
    }
}
```

Replace with:
```cpp
void punchmeter_set_config(const PunchmeterConfig *cfg) {
    if (cfg) {
        maxScore          = cfg->maxScore;
        minScore          = cfg->minScore;
        defaultRollDelay  = cfg->defaultRollDelay;
        rollDelayMod      = cfg->rollDelayMod;
        rollThresh        = cfg->rollThresh;
        slowRollThresh    = cfg->slowRollThresh;
        slowRollDelayMod  = cfg->slowRollDelayMod;
        defaultIncrement  = cfg->defaultIncrement;
        blinkDelay        = cfg->blinkDelay;
        waveDuration      = cfg->waveDuration;
        waveDelay         = cfg->waveDelay;
    }
}
```

**Step 2: Commit inside the submodule**

```bash
cd components/punchmeter
git add src/punchmeter.h src/punchmeter.cpp
git commit -m "Expose all interface values via PunchmeterConfig"
cd ../..
```

**Acceptance Criteria:**
- [ ] Run `cd components/punchmeter && git log -1 --stat`. Expected: one commit touching `punchmeter.h` and `punchmeter.cpp`.
- [ ] Run `cd components/punchmeter && git diff HEAD~1 HEAD -- src/punchmeter.cpp | grep -c "cfg->"`. Expected: 11.

---

## Task 3: Mirror the struct in `main/punchmeter_api.h`

**Files:**
- Modify: `main/punchmeter_api.h:12-16`

**Step 1: Replace the struct**

Current:
```cpp
// Mirrors PunchmeterConfig from punchmeter.h
struct PunchmeterConfig {
    unsigned long maxScore;  // microseconds, scores faster than this get 999 (default 20000)
    unsigned long minScore;  // microseconds, scores slower than this get 0 (default 500000)
};
```

Replace with:
```cpp
// Mirrors PunchmeterConfig from punchmeter.h — must stay in sync
struct PunchmeterConfig {
    unsigned long maxScore;
    unsigned long minScore;
    int defaultRollDelay;
    int rollDelayMod;
    int rollThresh;
    int slowRollThresh;
    int slowRollDelayMod;
    int defaultIncrement;
    int blinkDelay;
    int waveDuration;
    int waveDelay;
};
```

**Step 2: No commit yet** — bundled with all the main.cpp changes.

**Acceptance Criteria:**
- [ ] Read `main/punchmeter_api.h` — struct has the 11 fields in the same order as `components/punchmeter/src/punchmeter.h`.

---

## Task 4: Extend `shared_config_t` in `main/main.cpp` with the 11 fields + defaults

**Files:**
- Modify: `main/main.cpp:43-48`

**Step 1: Replace the struct and the default init**

Current:
```cpp
// Shared config protected by mutex
typedef struct {
    unsigned long maxScore;
    unsigned long minScore;
} shared_config_t;

static shared_config_t shared_config = { .maxScore = 20000, .minScore = 500000 };
```

Replace with:
```cpp
// Shared config protected by mutex
typedef struct {
    unsigned long maxScore;
    unsigned long minScore;
    int defaultRollDelay;
    int rollDelayMod;
    int rollThresh;
    int slowRollThresh;
    int slowRollDelayMod;
    int defaultIncrement;
    int blinkDelay;
    int waveDuration;
    int waveDelay;
} shared_config_t;

static shared_config_t shared_config = {
    .maxScore         = 20000,
    .minScore         = 100000,
    .defaultRollDelay = 15,
    .rollDelayMod     = 1,
    .rollThresh       = 60,
    .slowRollThresh   = 7,
    .slowRollDelayMod = 100,
    .defaultIncrement = 15,
    .blinkDelay       = 600,
    .waveDuration     = 1,
    .waveDelay        = 200,
};
```

**Note:** fixes the pre-existing `minScore = 500000` typo (punchmeter.cpp uses 100000 as the real default).

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `shared_config_t` has 11 fields and the initialiser sets all 11.

---

## Task 5: Extend the `punchmeter_task` config copy

**Files:**
- Modify: `main/main.cpp:128-134`

**Step 1: Replace the config copy block**

Current:
```cpp
// Check for config updates
if (xSemaphoreTake(config_mutex, 0) == pdTRUE) {
    PunchmeterConfig cfg;
    cfg.maxScore = shared_config.maxScore;
    cfg.minScore = shared_config.minScore;
    punchmeter_set_config(&cfg);
    xSemaphoreGive(config_mutex);
}
```

Replace with:
```cpp
// Check for config updates
if (xSemaphoreTake(config_mutex, 0) == pdTRUE) {
    PunchmeterConfig cfg;
    cfg.maxScore         = shared_config.maxScore;
    cfg.minScore         = shared_config.minScore;
    cfg.defaultRollDelay = shared_config.defaultRollDelay;
    cfg.rollDelayMod     = shared_config.rollDelayMod;
    cfg.rollThresh       = shared_config.rollThresh;
    cfg.slowRollThresh   = shared_config.slowRollThresh;
    cfg.slowRollDelayMod = shared_config.slowRollDelayMod;
    cfg.defaultIncrement = shared_config.defaultIncrement;
    cfg.blinkDelay       = shared_config.blinkDelay;
    cfg.waveDuration     = shared_config.waveDuration;
    cfg.waveDelay        = shared_config.waveDelay;
    punchmeter_set_config(&cfg);
    xSemaphoreGive(config_mutex);
}
```

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — the punchmeter_task block copies 11 fields from `shared_config` into `cfg`.

---

## Task 6: Extend POST `/api/config` parser with the 9 new fields

**Files:**
- Modify: `main/main.cpp:410-452` (`config_post_handler`)

**Step 1: Add a helper function above `config_post_handler`**

Insert before `config_post_handler`:

```cpp
// Parse a decimal integer following "\"key\":" in buf, returns true if found and updated.
static bool json_parse_int(const char *buf, const char *key, int *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(buf, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = atoi(colon + 1);
    return true;
}

static bool json_parse_ulong(const char *buf, const char *key, unsigned long *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(buf, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    unsigned long val = strtoul(colon + 1, NULL, 10);
    if (val == 0) return false;  // reject zero for ulong fields (existing behavior)
    *out = val;
    return true;
}
```

**Step 2: Replace the body of `config_post_handler`**

Current body (after `xSemaphoreTake`):
```cpp
char *key = strstr(buf, "\"maxScore\"");
if (key) { ... shared_config.maxScore = val; }

key = strstr(buf, "\"minScore\"");
if (key) { ... shared_config.minScore = val; }
```

Replace with:
```cpp
unsigned long ul_val;
int int_val;

if (json_parse_ulong(buf, "maxScore", &ul_val))         shared_config.maxScore         = ul_val;
if (json_parse_ulong(buf, "minScore", &ul_val))         shared_config.minScore         = ul_val;
if (json_parse_int  (buf, "defaultRollDelay", &int_val))shared_config.defaultRollDelay = int_val;
if (json_parse_int  (buf, "rollDelayMod", &int_val))    shared_config.rollDelayMod     = int_val;
if (json_parse_int  (buf, "rollThresh", &int_val))      shared_config.rollThresh       = int_val;
if (json_parse_int  (buf, "slowRollThresh", &int_val))  shared_config.slowRollThresh   = int_val;
if (json_parse_int  (buf, "slowRollDelayMod", &int_val))shared_config.slowRollDelayMod = int_val;
if (json_parse_int  (buf, "defaultIncrement", &int_val))shared_config.defaultIncrement = int_val;
if (json_parse_int  (buf, "blinkDelay", &int_val))      shared_config.blinkDelay       = int_val;
if (json_parse_int  (buf, "waveDuration", &int_val))    shared_config.waveDuration     = int_val;
if (json_parse_int  (buf, "waveDelay", &int_val))       shared_config.waveDelay        = int_val;

ESP_LOGI(TAG, "Config updated");
```

**Step 3: Increase the recv buffer**

Current: `char buf[128];` — too small for the full 11-field payload.

Replace with: `char buf[512];`

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `config_post_handler` parses 11 distinct keys.
- [ ] Read `main/main.cpp` — `buf` in `config_post_handler` is 512 bytes.

---

## Task 7: Add GET `/api/config` handler

**Files:**
- Modify: `main/main.cpp` (new handler above `start_webserver`)

**Step 1: Add the handler**

Insert just above `start_webserver`:

```cpp
static esp_err_t config_get_handler(httpd_req_t *req)
{
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"maxScore\":%lu,\"minScore\":%lu,"
        "\"defaultRollDelay\":%d,\"rollDelayMod\":%d,"
        "\"rollThresh\":%d,\"slowRollThresh\":%d,\"slowRollDelayMod\":%d,"
        "\"defaultIncrement\":%d,\"blinkDelay\":%d,"
        "\"waveDuration\":%d,\"waveDelay\":%d}",
        shared_config.maxScore, shared_config.minScore,
        shared_config.defaultRollDelay, shared_config.rollDelayMod,
        shared_config.rollThresh, shared_config.slowRollThresh, shared_config.slowRollDelayMod,
        shared_config.defaultIncrement, shared_config.blinkDelay,
        shared_config.waveDuration, shared_config.waveDelay);
    xSemaphoreGive(config_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}
```

**Step 2: Register the URI in `start_webserver`**

After the existing POST `config_uri` registration, add:

```cpp
const httpd_uri_t config_get_uri = {
    .uri = "/api/config",
    .method = HTTP_GET,
    .handler = config_get_handler,
    .user_ctx = NULL,
};
httpd_register_uri_handler(server, &config_get_uri);
```

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `config_get_handler` exists and is registered on `HTTP_GET /api/config`.

---

## Task 8: Add time-sync state + `compute_wall_clock` helper

**Files:**
- Modify: `main/main.cpp` (new statics + helper, above `add_score`)

**Step 1: Add the statics**

Insert just before `static void add_score(...)`:

```cpp
// ---------------------------------------------------------------------------
// Wall-clock sync — one-shot from backend register
// ---------------------------------------------------------------------------

static int     sync_hour = 0, sync_minute = 0, sync_second = 0;
static int64_t sync_boot_ms = 0;
static bool    time_synced = false;

static void compute_wall_clock(int *out_hour, int *out_minute)
{
    if (!time_synced) {
        int64_t ms = esp_timer_get_time() / 1000;
        int64_t total_min = ms / 60000;
        *out_hour   = (int)(total_min / 60) % 24;
        *out_minute = (int)(total_min % 60);
        return;
    }
    int64_t now_ms  = esp_timer_get_time() / 1000;
    int64_t delta_s = (now_ms - sync_boot_ms) / 1000;
    int64_t total_s = (int64_t)sync_hour * 3600
                    + (int64_t)sync_minute * 60
                    + sync_second + delta_s;
    int64_t day_s   = ((total_s % 86400) + 86400) % 86400;
    *out_hour   = (int)(day_s / 3600);
    *out_minute = (int)((day_s / 60) % 60);
}
```

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — statics and `compute_wall_clock` are present.

---

## Task 9: Use `compute_wall_clock` in `drain_score_queue`

**Files:**
- Modify: `main/main.cpp:157-166`

**Step 1: Replace the hour/minute computation**

Current:
```cpp
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

Replace with:
```cpp
static void drain_score_queue(void)
{
    score_event_t evt;
    while (xQueueReceive(score_queue, &evt, 0) == pdTRUE) {
        int hour, minute;
        compute_wall_clock(&hour, &minute);
        add_score(hour, minute, evt.score);
    }
}
```

**Note:** `timestamp_ms` is no longer used but keep the field in `score_event_t` — the queue producer in `punchmeter_task` still sets it, and removing it is a non-trivial refactor we don't need.

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `drain_score_queue` calls `compute_wall_clock`.

---

## Task 10: Parse `hour/minute/second` in `backend_register_handler`

**Files:**
- Modify: `main/main.cpp:635-693` (`backend_register_handler`)

**Step 1: After the existing IP parsing and storage (just before the `fetch_history_from_laptop()` call), add:**

```cpp
// Parse optional time fields and arm wall-clock sync
int h = -1, m = -1, s = -1;
json_parse_int(buf, "hour",   &h);
json_parse_int(buf, "minute", &m);
json_parse_int(buf, "second", &s);
if (h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60) {
    sync_hour    = h;
    sync_minute  = m;
    sync_second  = s;
    sync_boot_ms = esp_timer_get_time() / 1000;
    time_synced  = true;
    ESP_LOGI(TAG, "Wall clock synced: %02d:%02d:%02d", h, m, s);
}
```

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `backend_register_handler` parses `hour`/`minute`/`second` and sets `time_synced`.

---

## Task 11: Commit all main.cpp / punchmeter_api.h changes

**Step 1: Commit**

```bash
git add main/main.cpp main/punchmeter_api.h
git commit -m "Extend config API + add wall-clock sync from backend"
```

**Acceptance Criteria:**
- [ ] Run `git log -1 --stat`. Expected: one commit touching `main/main.cpp` and `main/punchmeter_api.h`.

---

## Task 12: Build firmware — verify compile

**Step 1: Build**

Run: `make build`

Expected: `Project build complete.` and a fresh `build/bapthit.bin`.

**Step 2: If build fails**, read the error, fix, rebuild until clean. Common issues:
- Missing `#include` (unlikely — all helpers stay in the same file).
- `snprintf` size warnings — bump buffer sizes.

**Acceptance Criteria:**
- [ ] Run `make build`. Expected: ends with `Project build complete.` and no errors.
- [ ] Run `ls -la build/bapthit.bin`. Expected: file exists, mtime ≈ now.

---

## Task 13: Send time in backend register payload

**Files:**
- Modify: `backend/server.py:38-49`

**Step 1: Import datetime (check if already imported)**

Ensure `from datetime import datetime` is at the top (add if missing).

**Step 2: Update the register call**

Current:
```python
async def register_with_esp32(my_ip: str):
    """Register this backend with the ESP32."""
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            await client.post(
                f"http://{ESP32_IP}/api/backend",
                json={"ip": my_ip},
            )
            print(f"Registered with ESP32 as {my_ip}")
    except Exception as e:
        print(f"Failed to register with ESP32: {e}")
        print("Will retry when ESP32 is available.")
```

Replace with:
```python
async def register_with_esp32(my_ip: str):
    """Register this backend with the ESP32 and sync its wall clock."""
    now = datetime.now()
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            await client.post(
                f"http://{ESP32_IP}/api/backend",
                json={
                    "ip": my_ip,
                    "hour": now.hour,
                    "minute": now.minute,
                    "second": now.second,
                },
            )
            print(f"Registered with ESP32 as {my_ip} at {now:%H:%M:%S}")
    except Exception as e:
        print(f"Failed to register with ESP32: {e}")
        print("Will retry when ESP32 is available.")
```

**Acceptance Criteria:**
- [ ] Read `backend/server.py` — `register_with_esp32` sends `hour`, `minute`, `second` in the JSON body.

---

## Task 14: Fix the photo button — label wrapper + `capture="user"`

**Files:**
- Modify: `main/index.html:603-614` (CSS) and `main/index.html:771-775` (HTML)

**Step 1: Update the CSS**

Find the `.claim-photo-btn` rule and add positioning + hide the contained input.

Current:
```css
.claim-photo-btn {
  /* existing rules */
}
.claim-photo-btn:hover { border-color: var(--red-dim); color: var(--text); }
```

Add after these rules (do not remove the existing ones — they already style the button shape):
```css
.claim-photo-btn {
  position: relative;
  overflow: hidden;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.claim-photo-btn input[type="file"] {
  position: absolute;
  inset: 0;
  opacity: 0;
  cursor: pointer;
  font-size: 0;  /* iOS: prevents the native button from leaking visual space */
}
```

**Step 2: Replace the button + hidden input markup**

Current:
```html
<div class="claim-photo-row" id="claim-photo-row">
  <button class="claim-photo-btn" onclick="document.getElementById('claim-photo-input').click()">Photo</button>
  <input type="file" id="claim-photo-input" accept="image/*" capture="environment" style="display:none" onchange="previewClaimPhoto(this)">
  <img class="claim-photo-preview" id="claim-photo-preview">
</div>
```

Replace with:
```html
<div class="claim-photo-row" id="claim-photo-row">
  <label class="claim-photo-btn">
    <span>Selfie</span>
    <input type="file" id="claim-photo-input" accept="image/*" capture="user" onchange="previewClaimPhoto(this)">
  </label>
  <img class="claim-photo-preview" id="claim-photo-preview">
</div>
```

**Acceptance Criteria:**
- [ ] Read `main/index.html` — the claim-photo-row uses a `<label>` wrapping the input; the input has `capture="user"` and no inline `display:none`.
- [ ] Flash/OTA the firmware, open the page on a phone on BAPTHIT Wi-Fi, tap an unclaimed score, tap "Selfie". Expected: the phone's native camera app opens on the front-facing camera.

---

## Task 15: Add PunchMeter config form in the Settings panel

**Files:**
- Modify: `main/index.html` around `settings-sheet` (settings section near line 742)

**Step 1: Add a CSS block for the form (before `</style>`)**

```css
.config-form {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 0.5rem 0.8rem;
  align-items: center;
  margin-top: 0.8rem;
}
.config-form label {
  font-size: 0.75rem;
  color: var(--text-dim);
  letter-spacing: 0.05em;
}
.config-form input[type="number"] {
  width: 110px;
  padding: 0.4rem 0.6rem;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text);
  font-family: 'Courier New', monospace;
  font-size: 0.85rem;
  text-align: right;
}
.config-form input[type="number"]:focus {
  border-color: var(--red-dim);
  outline: none;
}
.config-save-btn {
  grid-column: 1 / -1;
  margin-top: 0.6rem;
  padding: 0.7rem;
  background: var(--red);
  color: white;
  border: none;
  border-radius: 8px;
  font-weight: 700;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  font-size: 0.8rem;
  cursor: pointer;
}
.config-save-btn:hover { background: var(--red-glow); }
.config-save-btn:disabled { opacity: 0.5; cursor: default; }
.config-status {
  grid-column: 1 / -1;
  font-size: 0.7rem;
  text-align: center;
  min-height: 1em;
  color: var(--text-dim);
}
.config-status.success { color: #2ecc40; }
.config-status.error { color: var(--red); }
```

**Step 2: Insert the config section inside the settings sheet**

Insert just before the closing `</div>` of `.settings-sheet`, after the Backend status block:

```html
<div class="section-label" style="margin-top:1.5rem">Config PunchMeter</div>
<div class="config-form" id="config-form">
  <label for="cfg-maxScore">Max score (µs)</label>
  <input type="number" id="cfg-maxScore" min="1">
  <label for="cfg-minScore">Min score (µs)</label>
  <input type="number" id="cfg-minScore" min="1">
  <label for="cfg-defaultRollDelay">Roll delay (ms)</label>
  <input type="number" id="cfg-defaultRollDelay" min="0">
  <label for="cfg-rollDelayMod">Roll delay mod</label>
  <input type="number" id="cfg-rollDelayMod" min="0">
  <label for="cfg-rollThresh">Roll thresh</label>
  <input type="number" id="cfg-rollThresh" min="0">
  <label for="cfg-slowRollThresh">Slow roll thresh</label>
  <input type="number" id="cfg-slowRollThresh" min="0">
  <label for="cfg-slowRollDelayMod">Slow roll mod</label>
  <input type="number" id="cfg-slowRollDelayMod" min="0">
  <label for="cfg-defaultIncrement">Default increment</label>
  <input type="number" id="cfg-defaultIncrement" min="0">
  <label for="cfg-blinkDelay">Blink delay (ms)</label>
  <input type="number" id="cfg-blinkDelay" min="0">
  <label for="cfg-waveDuration">Wave duration</label>
  <input type="number" id="cfg-waveDuration" min="0">
  <label for="cfg-waveDelay">Wave delay (ms)</label>
  <input type="number" id="cfg-waveDelay" min="0">
  <button class="config-save-btn" id="config-save-btn" onclick="saveConfig()">Enregistrer</button>
  <div class="config-status" id="config-status"></div>
</div>
```

**Acceptance Criteria:**
- [ ] Read `main/index.html` — 11 number inputs with ids `cfg-maxScore`…`cfg-waveDelay` exist inside `.settings-sheet`.

---

## Task 16: Wire up GET/POST `/api/config` in the JS

**Files:**
- Modify: `main/index.html` (script block, near the `toggleSettings` function)

**Step 1: Add two new functions**

Insert after `function toggleSettings() { ... }`:

```javascript
const CONFIG_FIELDS = [
  'maxScore','minScore','defaultRollDelay','rollDelayMod','rollThresh',
  'slowRollThresh','slowRollDelayMod','defaultIncrement','blinkDelay',
  'waveDuration','waveDelay'
];

async function loadConfig() {
  try {
    const res = await fetch('/api/config');
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const cfg = await res.json();
    for (const k of CONFIG_FIELDS) {
      const el = document.getElementById('cfg-' + k);
      if (el && cfg[k] !== undefined) el.value = cfg[k];
    }
  } catch (e) {
    const status = document.getElementById('config-status');
    status.className = 'config-status error';
    status.textContent = 'Impossible de charger la config';
  }
}

async function saveConfig() {
  const btn = document.getElementById('config-save-btn');
  const status = document.getElementById('config-status');
  const payload = {};
  for (const k of CONFIG_FIELDS) {
    const el = document.getElementById('cfg-' + k);
    const v = parseInt(el.value, 10);
    if (!Number.isFinite(v)) {
      status.className = 'config-status error';
      status.textContent = 'Valeur invalide: ' + k;
      return;
    }
    payload[k] = v;
  }
  btn.disabled = true;
  status.className = 'config-status';
  status.textContent = '...';
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    status.className = 'config-status success';
    status.textContent = 'Enregistre';
  } catch (e) {
    status.className = 'config-status error';
    status.textContent = 'Erreur: ' + e.message;
  } finally {
    btn.disabled = false;
  }
}
```

**Step 2: Call `loadConfig()` when the settings panel opens**

Update `toggleSettings`:

Current:
```javascript
function toggleSettings() {
  const overlay = document.getElementById('settings-overlay');
  const gear = document.getElementById('gear-btn');
  overlay.classList.toggle('active');
  gear.classList.toggle('active');
}
```

Replace with:
```javascript
function toggleSettings() {
  const overlay = document.getElementById('settings-overlay');
  const gear = document.getElementById('gear-btn');
  const wasActive = overlay.classList.contains('active');
  overlay.classList.toggle('active');
  gear.classList.toggle('active');
  if (!wasActive) loadConfig();
}
```

**Acceptance Criteria:**
- [ ] Read `main/index.html` — `loadConfig`, `saveConfig`, and the updated `toggleSettings` are present.

---

## Task 17: Commit frontend + backend changes

**Step 1: Commit**

```bash
git add main/index.html backend/server.py
git commit -m "Add config form, selfie fix, time sync client"
```

**Acceptance Criteria:**
- [ ] Run `git log -1 --stat`. Expected: one commit touching `main/index.html` and `backend/server.py`.

---

## Task 18: Rebuild firmware with the new `index.html` embedded

**Step 1: Build**

Run: `make build`

Expected: `Project build complete.` with no errors.

**Step 2: Verify the new binary exists**

Run: `ls -la build/bapthit.bin`

**Acceptance Criteria:**
- [ ] Run `make build`. Expected: ends with `Project build complete.` and no errors.
- [ ] Run `ls -la build/bapthit.bin`. Expected: fresh mtime.

---

## Task 19: Update submodule pointer in parent repo

**Step 1: Check parent repo sees the submodule change**

Run: `git status`

Expected output should include `modified: components/punchmeter (new commits)`.

**Step 2: Commit the pointer update**

```bash
git add components/punchmeter
git commit -m "Bump punchmeter to expose all interface values"
```

**Acceptance Criteria:**
- [ ] Run `git log -1 --stat`. Expected: one commit touching `components/punchmeter`.
- [ ] Run `git submodule status`. Expected: clean (no `+` prefix).

---

## Final smoke test (manual, post-flash)

These cannot all be automated — execute after OTA/flash.

- [ ] Start the backend (`make backend`). Expected log: `Registered with ESP32 as <ip> at HH:MM:SS`.
- [ ] Curl GET config: `curl http://192.168.4.1/api/config`. Expected: JSON with all 11 fields reflecting defaults (or previously POSTed values).
- [ ] Curl POST config: `curl -X POST http://192.168.4.1/api/config -H 'Content-Type: application/json' -d '{"blinkDelay":300}'`. Expected: `{"ok":true}`.
- [ ] Re-GET: blinkDelay should now be 300.
- [ ] On phone on BAPTHIT: open page, tap unclaimed score, tap Selfie button. Expected: front camera app opens.
- [ ] After taking a photo and submitting: the photo appears next to the score entry (requires backend reachable).
- [ ] Trigger a new score (`make serial-score SCORE=500`). Expected: the score row shows the real wall-clock time from the laptop, not the uptime-based one.

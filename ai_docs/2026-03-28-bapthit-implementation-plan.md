# Bapt'Hit Web App Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a punching bag score tracking web app hosted on the ESP32 in AP mode, with a dark boxing-themed UI featuring Scores and Stats tabs.

**Architecture:** ESP32 creates a Wi-Fi AP ("BAPTHIT"), runs `esp_http_server` serving a single embedded HTML page and a JSON API endpoint. Scores are stored in an in-memory array with mock data generated at boot. The web app polls `/api/scores` every 3 seconds.

**Tech Stack:** ESP-IDF v6.0, `esp_wifi` (AP mode), `esp_http_server`, `esp_event`, `nvs_flash`, embedded HTML/CSS/JS via `EMBED_TXTFILES`

---

### Task 1: Wi-Fi AP Setup

**Files:**
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`
- Modify: `sdkconfig.defaults`

**Step 1: Add Wi-Fi AP config to sdkconfig.defaults**

Append to `sdkconfig.defaults`:
```
# Wi-Fi AP
CONFIG_ESP_WIFI_SSID="BAPTHIT"
CONFIG_ESP_WIFI_PASSWORD=""
CONFIG_ESP_WIFI_CHANNEL=1
CONFIG_ESP_WIFI_MAX_STA_NUM=8
```

**Step 2: Implement Wi-Fi AP initialization in main.c**

Replace `main/main.c` with Wi-Fi AP setup code:
- Initialize NVS (required by Wi-Fi)
- Initialize TCP/IP stack (`esp_netif_init`)
- Create default event loop (`esp_event_loop_create_default`)
- Create AP netif (`esp_netif_create_default_wifi_ap`)
- Configure Wi-Fi in AP mode with SSID "BAPTHIT", open auth (no password), max 8 stations
- Start Wi-Fi

Keep the existing OTA validation logic.

**Step 3: Update CMakeLists.txt**

Add required components:
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_server esp_app_format app_update
)
```

**Step 4: Build and flash**

```bash
export IDF_PATH=$HOME/.espressif/v6.0/esp-idf && . $IDF_PATH/export.sh
idf.py fullclean && idf.py set-target esp32 && idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

**Step 5: Commit**

```bash
git add main/main.c main/CMakeLists.txt sdkconfig.defaults
git commit -m "feat: add Wi-Fi AP mode (BAPTHIT network)"
```

**Acceptance Criteria:**
- [ ] **Connect phone to Wi-Fi**: Scan Wi-Fi on phone, "BAPTHIT" network appears, connect without password
      Expected: Phone connects successfully, ESP32 monitor logs "station connected"
- [ ] **Check monitor output**: Read ESP32 serial monitor output
      Expected: Logs show "Wi-Fi AP started" with SSID "BAPTHIT" and IP info

---

### Task 2: HTTP Server + Score Store + JSON API

**Files:**
- Modify: `main/main.c`

**Step 1: Add score data structures and mock data**

Define at top of `main.c`:
```c
#define MAX_SCORES 64

typedef struct {
    int hour;
    int minute;
    int score;
} score_entry_t;

static score_entry_t scores[MAX_SCORES];
static int score_count = 0;
```

Implement `generate_mock_scores()` that fills the array with ~15 random scores between 500-1200, spread across hours 13-22.

**Step 2: Implement JSON API handler**

Create `scores_get_handler` for `GET /api/scores`:
- Build JSON string manually (no cJSON dependency): `{"scores":[{"time":"20h01","score":999},...]}`
- Set content type to `application/json`
- Set CORS header `Access-Control-Allow-Origin: *`

**Step 3: Start HTTP server**

Create `start_webserver()`:
- Configure `httpd_config_t` with default settings
- Register URI handler for `/api/scores`
- Call from `app_main()` after Wi-Fi is started

**Step 4: Build and flash**

```bash
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```

**Step 5: Verify API endpoint**

Connect phone to BAPTHIT Wi-Fi, open browser to `http://192.168.4.1/api/scores`

**Step 6: Commit**

```bash
git add main/main.c
git commit -m "feat: add HTTP server with /api/scores JSON endpoint and mock data"
```

**Acceptance Criteria:**
- [ ] **Curl API endpoint**: Connect to BAPTHIT Wi-Fi, run `curl http://192.168.4.1/api/scores`
      Expected: Returns valid JSON with ~15 score entries, each having "time" and "score" fields
- [ ] **Check JSON format**: Inspect the returned JSON
      Expected: Format is `{"scores":[{"time":"20h01","score":999},...]}` with times in HHhMM format

---

### Task 3: HTML/CSS/JS Web App (Scores + Stats tabs)

**Files:**
- Create: `main/index.html`
- Modify: `main/CMakeLists.txt`
- Modify: `main/main.c`

**Step 1: Create the single-page HTML file**

Create `main/index.html` with:

**HTML structure:**
- Header: "BAPT'HIT" title
- Tab bar: "Scores" | "Stats" (clickable, underline active tab)
- Scores panel: "Derniers Scores" section + "Meilleurs Scores" section
- Stats panel: `<canvas>` element for the chart

**CSS (dark boxing theme):**
- Background: near-black (#0a0a0a)
- Accent color: bold red (#ff1a1a) or neon red
- Typography: bold, uppercase, impact-style font stack
- Score entries: monospace time, large score numbers
- Mobile-first: full viewport, no scroll bars on main container
- Rounded card containers with subtle dark borders
- Glowing/neon accent effects on the title

**JavaScript:**
- `fetchScores()`: GET `/api/scores`, parse JSON, update both tabs
- `renderScores(scores)`: populate "Derniers Scores" (last 10, reverse chronological) and "Meilleurs Scores" (top 5, sorted by score desc)
- `renderChart(scores)`: draw on canvas — X axis (time labels), Y axis (score), individual dots + smoothed bezier trend line
- Tab switching: toggle visibility of scores/stats panels
- Auto-poll every 3 seconds via `setInterval`

**Step 2: Embed HTML in flash**

Update `main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_server esp_app_format app_update
    EMBED_TXTFILES "index.html"
)
```

**Step 3: Add root URI handler**

In `main.c`, add handler for `GET /`:
```c
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}
```

Register this handler in `start_webserver()`.

**Step 4: Build and flash**

```bash
idf.py build && idf.py -p /dev/ttyUSB0 flash monitor
```

**Step 5: Commit**

```bash
git add main/index.html main/CMakeLists.txt main/main.c
git commit -m "feat: add dark boxing-themed web app with Scores and Stats tabs"
```

**Acceptance Criteria:**
- [ ] **Open web app**: Connect phone to BAPTHIT, open `http://192.168.4.1/`
      Expected: Dark themed page loads with "BAPT'HIT" title, Scores tab active by default
- [ ] **Check Scores tab**: View the Scores tab content
      Expected: "Derniers Scores" shows recent scores with times, "Meilleurs Scores" shows top scores sorted by value
- [ ] **Switch to Stats tab**: Tap "Stats" tab
      Expected: Line chart renders with individual score dots and a smoothed trend line, time axis labels visible
- [ ] **Auto-refresh**: Wait 5+ seconds on the Scores tab
      Expected: Data refreshes without manual page reload (scores re-render from API poll)
- [ ] **Mobile layout**: View on phone in portrait orientation
      Expected: Full-screen, no horizontal scrolling, readable text, chart fills available width

---

### Task 4: Final verification and cleanup

**Files:**
- Modify: `main/main.c` (if any fixes needed)

**Step 1: End-to-end test on device**

Flash final firmware, connect phone, test both tabs, verify auto-refresh, check monitor for errors.

**Step 2: Check firmware size**

```bash
idf.py size-components
```

Verify app fits comfortably in the 1.25MB OTA partition.

**Step 3: Commit any fixes**

```bash
git add -A && git commit -m "fix: final polish and cleanup"
git push
```

**Acceptance Criteria:**
- [ ] **Firmware size**: Run `idf.py size-components`
      Expected: Binary well under 1.25MB (0x140000 bytes), leaving room for growth
- [ ] **No errors in monitor**: Run `idf.py monitor` during web app usage
      Expected: No error logs, clean INFO-level output
- [ ] **Multiple clients**: Connect 2+ devices to BAPTHIT simultaneously
      Expected: Both see the web app and receive score data

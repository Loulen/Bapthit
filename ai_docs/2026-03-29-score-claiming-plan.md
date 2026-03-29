# Score Claiming & Laptop Backend Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Allow users to claim leaderboard scores with their name and photo, with optional laptop backend for persistence and photo storage.

**Architecture:** ESP32 stays the primary system — serves UI, manages scores in memory, handles claims. A laptop on the BAPTHIT WiFi optionally provides persistence (SQLite) and photo storage. The laptop registers itself with the ESP32; the ESP32 forwards data to it. Phones upload photos directly to the laptop to avoid routing large files through the ESP32.

**Tech Stack:** ESP-IDF v6.0 (C/C++), esp_http_client for forwarding, Python FastAPI + SQLite for laptop server, vanilla JS for web UI.

---

### Task 1: Extend Score Data Model

**Files:**
- Modify: `main/main.cpp:34-72` (score_entry_t, add_score, globals)

**Step 1: Update score_entry_t and globals**

Replace the score store section (`main/main.cpp:49-72`) with:

```c
#define MAX_SCORES 64
#define MAX_NAME_LEN 32

typedef struct {
    int id;
    int hour;
    int minute;
    int score;
    char name[MAX_NAME_LEN];  // empty = unclaimed
    bool has_photo;
} score_entry_t;

static score_entry_t scores[MAX_SCORES];
static int score_count = 0;
static int next_score_id = 1;

// Laptop backend (empty = not registered)
static char laptop_ip[16] = "";

static void add_score(int hour, int minute, int score)
{
    if (score_count < MAX_SCORES) {
        scores[score_count].id = next_score_id++;
        scores[score_count].hour = hour;
        scores[score_count].minute = minute;
        scores[score_count].score = score;
        scores[score_count].name[0] = '\0';
        scores[score_count].has_photo = false;
        score_count++;
    }
}
```

**Step 2: Update JSON response in scores_get_handler**

Replace `scores_get_handler` (`main/main.cpp:124-148`) with:

```c
static esp_err_t scores_get_handler(httpd_req_t *req)
{
    drain_score_queue();

    // Larger buffer: id + name + has_photo per entry
    char *buf = (char *)malloc(8192);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = sprintf(buf, "{\"scores\":[");
    for (int i = 0; i < score_count; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += sprintf(buf + pos,
            "{\"id\":%d,\"time\":\"%02dh%02d\",\"score\":%d,\"name\":\"%s\",\"has_photo\":%s}",
            scores[i].id,
            scores[i].hour, scores[i].minute,
            scores[i].score,
            scores[i].name,
            scores[i].has_photo ? "true" : "false");
    }
    pos += sprintf(buf + pos, "],\"laptop_ip\":%s%s%s}",
        laptop_ip[0] ? "\"" : "null",
        laptop_ip[0] ? laptop_ip : "",
        laptop_ip[0] ? "\"" : "");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}
```

**Step 3: Build and verify**

Run: `idf.py build`
Expected: Compiles with no errors.

**Step 4: Flash and test JSON response**

Run: `idf.py flash monitor`
Then inject a test score via serial: `SCORE:750`
Then from a phone/laptop on the BAPTHIT network:
```bash
curl http://192.168.4.1/api/scores
```
Expected: `{"scores":[{"id":1,"time":"...","score":750,"name":"","has_photo":false}],"laptop_ip":null}`

**Step 5: Commit**

```bash
git add main/main.cpp
git commit -m "feat: extend score model with id, name, has_photo and laptop_ip"
```

**Acceptance Criteria:**

- [ ] **curl**: `GET /api/scores` returns JSON with `id`, `name`, `has_photo` fields per score and `laptop_ip` at root level
      Expected: `{"scores":[{"id":1,"time":"...","score":750,"name":"","has_photo":false}],"laptop_ip":null}`
- [ ] **Serial inject**: Send `SCORE:800` then `SCORE:600`, verify IDs are sequential (1, 2)
      Expected: Two scores with `id:1` and `id:2`

---

### Task 2: Add Claim Endpoint

**Files:**
- Modify: `main/main.cpp` (new handler + register route)

**Step 1: Add claim handler**

Add after `scores_get_handler`:

```c
static esp_err_t claim_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse "id" field
    char *id_key = strstr(buf, "\"id\"");
    if (!id_key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }
    char *colon = strchr(id_key, ':');
    int id = atoi(colon + 1);

    // Parse "name" field
    char *name_key = strstr(buf, "\"name\"");
    if (!name_key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    char *quote1 = strchr(strchr(name_key, ':'), '"');
    if (!quote1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
        return ESP_FAIL;
    }
    quote1++; // skip opening quote
    char *quote2 = strchr(quote1, '"');
    if (!quote2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
        return ESP_FAIL;
    }

    // Find score by ID
    int idx = -1;
    for (int i = 0; i < score_count; i++) {
        if (scores[i].id == id) { idx = i; break; }
    }
    if (idx < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Score not found\"}");
        return ESP_OK;
    }
    if (scores[idx].name[0] != '\0') {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Already claimed\"}");
        return ESP_OK;
    }

    // Set name
    int name_len = quote2 - quote1;
    if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;
    memcpy(scores[idx].name, quote1, name_len);
    scores[idx].name[name_len] = '\0';

    ESP_LOGI(TAG, "Score %d claimed by '%s'", id, scores[idx].name);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

**Step 2: Register the route in start_webserver**

Add before the captive portal catch-all registration:

```c
const httpd_uri_t claim_uri = {
    .uri = "/api/claim",
    .method = HTTP_POST,
    .handler = claim_post_handler,
    .user_ctx = NULL,
};
httpd_register_uri_handler(server, &claim_uri);
```

**Step 3: Build, flash, and test**

Run: `idf.py build && idf.py flash monitor`
Inject a score: `SCORE:750`
Then:
```bash
# Claim it
curl -X POST http://192.168.4.1/api/claim -d '{"id":1,"name":"Lucas"}'
# Expected: {"ok":true}

# Verify it shows up
curl http://192.168.4.1/api/scores
# Expected: name:"Lucas" on id:1

# Try claiming again
curl -X POST http://192.168.4.1/api/claim -d '{"id":1,"name":"Other"}'
# Expected: 409 {"error":"Already claimed"}

# Try non-existent
curl -X POST http://192.168.4.1/api/claim -d '{"id":99,"name":"Test"}'
# Expected: 404 {"error":"Score not found"}
```

**Step 4: Commit**

```bash
git add main/main.cpp
git commit -m "feat: add POST /api/claim endpoint for score claiming"
```

**Acceptance Criteria:**

- [ ] **curl**: `POST /api/claim {"id":1,"name":"Lucas"}` returns `{"ok":true}`
      Expected: 200 with ok:true, subsequent `GET /api/scores` shows `"name":"Lucas"` on that score
- [ ] **curl**: Claiming an already-claimed score returns 409
      Expected: `{"error":"Already claimed"}`
- [ ] **curl**: Claiming a non-existent ID returns 404
      Expected: `{"error":"Score not found"}`

---

### Task 3: Add Backend Registration and Photo Confirmation Endpoints

**Files:**
- Modify: `main/main.cpp` (two new handlers + register routes)

**Step 1: Add backend register handler**

```c
static esp_err_t backend_register_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse "ip" field
    char *ip_key = strstr(buf, "\"ip\"");
    if (!ip_key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ip");
        return ESP_FAIL;
    }
    char *quote1 = strchr(strchr(ip_key, ':'), '"');
    if (!quote1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ip");
        return ESP_FAIL;
    }
    quote1++;
    char *quote2 = strchr(quote1, '"');
    if (!quote2 || (quote2 - quote1) >= (int)sizeof(laptop_ip)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ip");
        return ESP_FAIL;
    }

    int ip_len = quote2 - quote1;
    memcpy(laptop_ip, quote1, ip_len);
    laptop_ip[ip_len] = '\0';

    ESP_LOGI(TAG, "Backend registered at %s", laptop_ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

**Step 2: Add photo_ok handler**

```c
static esp_err_t photo_ok_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char *id_key = strstr(buf, "\"id\"");
    if (!id_key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }
    int id = atoi(strchr(id_key, ':') + 1);

    for (int i = 0; i < score_count; i++) {
        if (scores[i].id == id) {
            scores[i].has_photo = true;
            ESP_LOGI(TAG, "Photo confirmed for score %d", id);
            break;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

**Step 3: Register routes in start_webserver**

Add before the captive portal catch-all:

```c
const httpd_uri_t backend_uri = {
    .uri = "/api/backend",
    .method = HTTP_POST,
    .handler = backend_register_handler,
    .user_ctx = NULL,
};
httpd_register_uri_handler(server, &backend_uri);

const httpd_uri_t photo_ok_uri = {
    .uri = "/api/photo_ok",
    .method = HTTP_POST,
    .handler = photo_ok_handler,
    .user_ctx = NULL,
};
httpd_register_uri_handler(server, &photo_ok_uri);
```

**Step 4: Increase max_uri_handlers**

In `start_webserver`, change:
```c
config.max_uri_handlers = 16;  // was 10
```

**Step 5: Build, flash, and test**

```bash
# Register a fake backend
curl -X POST http://192.168.4.1/api/backend -d '{"ip":"192.168.4.2"}'
# Expected: {"ok":true}

# Verify laptop_ip appears in scores response
curl http://192.168.4.1/api/scores
# Expected: "laptop_ip":"192.168.4.2"

# Confirm a photo (after injecting a score and claiming it)
curl -X POST http://192.168.4.1/api/photo_ok -d '{"id":1}'
# Expected: {"ok":true}, GET /api/scores shows has_photo:true
```

**Step 6: Commit**

```bash
git add main/main.cpp
git commit -m "feat: add backend registration and photo confirmation endpoints"
```

**Acceptance Criteria:**

- [ ] **curl**: `POST /api/backend {"ip":"192.168.4.2"}` returns ok, `GET /api/scores` includes `"laptop_ip":"192.168.4.2"`
      Expected: laptop_ip field populated in scores response
- [ ] **curl**: `POST /api/photo_ok {"id":1}` sets `has_photo:true` on that score
      Expected: Subsequent `GET /api/scores` shows `"has_photo":true` for score id 1
- [ ] **serial monitor**: Backend registration logs `"Backend registered at 192.168.4.2"`
      Expected: Log line visible in serial output

---

### Task 4: Forward Scores and Claims to Laptop

**Files:**
- Modify: `main/main.cpp` (forwarding task + queue)
- Modify: `main/CMakeLists.txt` (add esp_http_client dependency)

**Step 1: Add esp_http_client dependency**

In `main/CMakeLists.txt`, add `esp_http_client` to the REQUIRES list:

```
REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_server esp_http_client esp_app_format app_update lwip idf_component
```

**Step 2: Add include and forwarding types**

Add near the top of `main/main.cpp` after existing includes:

```c
#include "esp_http_client.h"
```

Add after the laptop_ip global:

```c
// Forwarding queue to laptop backend
#define FWD_QUEUE_SIZE 16

typedef enum {
    FWD_SCORE,
    FWD_CLAIM,
} fwd_type_t;

typedef struct {
    fwd_type_t type;
    int id;
    int hour;
    int minute;
    int score;
    char name[MAX_NAME_LEN];
} fwd_event_t;

static QueueHandle_t fwd_queue = NULL;
```

**Step 3: Add forwarding task**

```c
static void forward_to_laptop(const char *path, const char *body)
{
    if (laptop_ip[0] == '\0') return;

    char url[64];
    snprintf(url, sizeof(url), "http://%s:8000%s", laptop_ip, path);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 3000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Forward to %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void fwd_task(void *arg)
{
    fwd_event_t evt;
    char body[192];

    while (1) {
        if (xQueueReceive(fwd_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (laptop_ip[0] == '\0') continue;

            switch (evt.type) {
            case FWD_SCORE:
                snprintf(body, sizeof(body),
                    "{\"id\":%d,\"hour\":%d,\"minute\":%d,\"score\":%d}",
                    evt.id, evt.hour, evt.minute, evt.score);
                forward_to_laptop("/api/scores", body);
                break;
            case FWD_CLAIM:
                snprintf(body, sizeof(body),
                    "{\"id\":%d,\"name\":\"%s\"}", evt.id, evt.name);
                forward_to_laptop("/api/scores/claim", body);
                break;
            }
        }
    }
}
```

**Step 4: Queue forwarding events from existing handlers**

In `add_score`, after the score is added (before the closing `}`):

```c
// Forward to laptop
if (fwd_queue) {
    fwd_event_t fwd = { .type = FWD_SCORE };
    fwd.id = scores[score_count - 1].id;
    fwd.hour = hour;
    fwd.minute = minute;
    fwd.score = score;
    xQueueSend(fwd_queue, &fwd, 0);
}
```

In `claim_post_handler`, after setting the name and before the log line:

```c
// Forward to laptop
if (fwd_queue) {
    fwd_event_t fwd = { .type = FWD_CLAIM };
    fwd.id = id;
    strncpy(fwd.name, scores[idx].name, MAX_NAME_LEN - 1);
    xQueueSend(fwd_queue, &fwd, 0);
}
```

**Step 5: Create queue and task in app_main**

Add after the `config_mutex` creation:

```c
// Create forwarding queue and task
fwd_queue = xQueueCreate(FWD_QUEUE_SIZE, sizeof(fwd_event_t));
xTaskCreate(fwd_task, "fwd_task", 4096, NULL, 3, NULL);
```

**Step 6: Build and verify**

Run: `idf.py build`
Expected: Compiles with no errors.

**Step 7: Commit**

```bash
git add main/main.cpp main/CMakeLists.txt
git commit -m "feat: forward scores and claims to laptop backend"
```

**Acceptance Criteria:**

- [ ] **build**: `idf.py build` succeeds with esp_http_client linked
      Expected: No compilation or link errors
- [ ] **serial monitor + laptop**: Register a backend, inject a score, observe the ESP32 attempts to POST to laptop:8000
      Expected: Either successful forward or `"Forward to ... failed"` warning in logs (confirming the path executes)

---

### Task 5: Boot Recovery from Laptop

**Files:**
- Modify: `main/main.cpp` (fetch history in backend_register_handler)

**Step 1: Add history fetch function**

Add before `backend_register_handler`:

```c
static void fetch_history_from_laptop(void)
{
    if (laptop_ip[0] == '\0') return;

    char url[64];
    snprintf(url, sizeof(url), "http://%s:8000/api/scores/history", laptop_ip);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 5000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "History fetch failed to open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > 8192) {
        ESP_LOGW(TAG, "History: bad content length %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    char *buf = (char *)malloc(content_length + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int read_len = esp_http_client_read(client, buf, content_length);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        free(buf);
        return;
    }
    buf[read_len] = '\0';

    // Simple JSON array parser for:
    // [{"id":1,"hour":14,"minute":32,"score":750,"name":"Lucas","has_photo":true}, ...]
    score_count = 0;
    next_score_id = 1;

    char *p = buf;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        if (score_count >= MAX_SCORES) break;

        score_entry_t *s = &scores[score_count];
        memset(s, 0, sizeof(*s));

        // Parse id
        s->id = atoi(strchr(p, ':') + 1);
        if (s->id >= next_score_id) next_score_id = s->id + 1;

        // Parse hour
        char *h = strstr(p, "\"hour\"");
        if (h) s->hour = atoi(strchr(h, ':') + 1);

        // Parse minute
        char *m = strstr(p, "\"minute\"");
        if (m) s->minute = atoi(strchr(m, ':') + 1);

        // Parse score
        char *sc = strstr(p, "\"score\"");
        if (sc) s->score = atoi(strchr(sc, ':') + 1);

        // Parse name
        char *n = strstr(p, "\"name\"");
        if (n) {
            char *q1 = strchr(strchr(n, ':'), '"');
            if (q1) {
                q1++;
                char *q2 = strchr(q1, '"');
                if (q2) {
                    int len = q2 - q1;
                    if (len >= MAX_NAME_LEN) len = MAX_NAME_LEN - 1;
                    memcpy(s->name, q1, len);
                    s->name[len] = '\0';
                }
            }
        }

        // Parse has_photo
        char *hp = strstr(p, "\"has_photo\"");
        if (hp) {
            char *val = strchr(hp, ':') + 1;
            while (*val == ' ') val++;
            s->has_photo = (strncmp(val, "true", 4) == 0);
        }

        score_count++;
        p++; // advance past current match
    }

    ESP_LOGI(TAG, "Loaded %d scores from backend history", score_count);
    free(buf);
}
```

**Step 2: Call fetch_history from backend_register_handler**

In `backend_register_handler`, after `ESP_LOGI(TAG, "Backend registered at %s", laptop_ip);`, add:

```c
// Fetch persisted scores from laptop
fetch_history_from_laptop();
```

**Step 3: Build and test**

Run: `idf.py build`
Expected: Compiles with no errors.

Integration test: Start the laptop server (Task 8) with some pre-existing scores, then register it with the ESP32. The ESP32 should populate its scores array.

**Step 4: Commit**

```bash
git add main/main.cpp
git commit -m "feat: fetch persisted score history when laptop registers"
```

**Acceptance Criteria:**

- [ ] **build**: `idf.py build` succeeds
      Expected: No errors
- [ ] **integration**: With laptop server running and containing scores, register backend with ESP32, then `GET /api/scores` returns the persisted scores
      Expected: Scores from laptop appear in ESP32's response after registration

---

### Task 6: Web UI — Claim Modal and Unclaimed Score Display

**Files:**
- Modify: `main/index.html` (CSS for modal + claim UI, JS for claim flow)

**Step 1: Add modal CSS**

Add before `</style>` (after the `.ota-status.error` rule):

```css
/* ---- CLAIM MODAL ---- */

.claim-overlay {
  display: none;
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.8);
  z-index: 60;
  animation: fadeIn 0.2s ease;
}

.claim-overlay.active { display: flex; align-items: center; justify-content: center; }

.claim-sheet {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 1.5rem;
  width: 90%;
  max-width: 340px;
}

.claim-title {
  font-family: Impact, 'Arial Black', sans-serif;
  font-size: 1.2rem;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--text);
  margin-bottom: 0.3rem;
}

.claim-score-display {
  font-family: Impact, 'Arial Black', sans-serif;
  font-size: 2rem;
  color: var(--red);
  text-shadow: 0 0 10px rgba(232,26,26,0.4);
  margin-bottom: 1rem;
}

.claim-input {
  width: 100%;
  padding: 0.7rem 1rem;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 8px;
  color: var(--text);
  font-size: 1rem;
  font-family: inherit;
  outline: none;
  transition: border-color 0.2s;
}

.claim-input:focus { border-color: var(--red-dim); }

.claim-input::placeholder { color: var(--text-muted); }

.claim-photo-row {
  display: none;
  margin-top: 0.8rem;
  align-items: center;
  gap: 0.8rem;
}

.claim-photo-row.visible { display: flex; }

.claim-photo-btn {
  padding: 0.5rem 1rem;
  background: var(--surface-alt);
  border: 1px solid var(--border);
  border-radius: 8px;
  color: var(--text-dim);
  font-size: 0.8rem;
  cursor: pointer;
  transition: all 0.2s;
}

.claim-photo-btn:hover { border-color: var(--red-dim); color: var(--text); }

.claim-photo-preview {
  width: 40px;
  height: 40px;
  border-radius: 6px;
  object-fit: cover;
  display: none;
  border: 1px solid var(--border);
}

.claim-actions {
  display: flex;
  gap: 0.6rem;
  margin-top: 1.2rem;
}

.claim-btn {
  flex: 1;
  padding: 0.7rem;
  border-radius: 8px;
  font-size: 0.85rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  cursor: pointer;
  transition: all 0.2s;
  border: 1px solid var(--border);
}

.claim-btn.cancel {
  background: transparent;
  color: var(--text-dim);
}

.claim-btn.cancel:hover { border-color: var(--text-muted); color: var(--text); }

.claim-btn.save {
  background: var(--red);
  border-color: var(--red);
  color: #fff;
}

.claim-btn.save:hover { background: var(--red-glow); }

.claim-btn:disabled {
  opacity: 0.4;
  pointer-events: none;
}

/* Unclaimed score styling */
.score-entry.unclaimed { cursor: pointer; }
.score-entry.unclaimed:hover { border-color: var(--red); }

.score-name {
  font-size: 0.8rem;
  color: var(--text-dim);
  margin-left: 0.6rem;
  max-width: 5rem;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.score-name.anonymous {
  color: var(--text-muted);
  font-style: italic;
}

.score-avatar {
  width: 28px;
  height: 28px;
  border-radius: 50%;
  object-fit: cover;
  margin-left: 0.5rem;
  border: 1px solid var(--border);
}
```

**Step 2: Add claim modal HTML**

Add before the `<div class="live-badge">` line:

```html
<div class="claim-overlay" id="claim-overlay" onclick="if(event.target===this)closeClaim()">
  <div class="claim-sheet">
    <div class="claim-title">Revendiquer ce score</div>
    <div class="claim-score-display" id="claim-score-display">-</div>
    <input class="claim-input" id="claim-name" type="text" placeholder="Ton nom..." maxlength="31" autocomplete="off">
    <div class="claim-photo-row" id="claim-photo-row">
      <button class="claim-photo-btn" onclick="document.getElementById('claim-photo-input').click()">Photo</button>
      <input type="file" id="claim-photo-input" accept="image/*" capture="environment" style="display:none" onchange="previewClaimPhoto(this)">
      <img class="claim-photo-preview" id="claim-photo-preview">
    </div>
    <div class="claim-actions">
      <button class="claim-btn cancel" onclick="closeClaim()">Annuler</button>
      <button class="claim-btn save" id="claim-save-btn" onclick="saveClaim()" disabled>Valider</button>
    </div>
  </div>
</div>
```

**Step 3: Update renderScores in JavaScript**

Replace the `renderScores` function with a version that shows names and makes unclaimed scores tappable. Store `laptopIp` globally from the fetch response.

```javascript
let allScores = [];
let laptopIp = null;
let claimingId = null;
let claimPhotoFile = null;

function renderScores(scores) {
  // Recent: last 10, reverse chronological
  const recent = scores.slice().reverse().slice(0, 10);
  const recentEl = document.getElementById('recent-scores');
  if (recent.length === 0) {
    recentEl.innerHTML = '<div class="empty">En attente des scores...</div>';
  } else {
    recentEl.innerHTML = recent.map(s => {
      const unclaimed = !s.name;
      const nameHtml = unclaimed
        ? '<span class="score-name anonymous">???</span>'
        : '<span class="score-name">' + esc(s.name) + '</span>';
      const avatarHtml = (s.has_photo && laptopIp)
        ? '<img class="score-avatar" src="http://' + laptopIp + ':8000/api/photos/' + s.id + '">'
        : '';
      return '<div class="score-entry' + (unclaimed ? ' unclaimed' : '') + '"'
        + (unclaimed ? ' onclick="openClaim(' + s.id + ',' + s.score + ')"' : '') + '>'
        + avatarHtml
        + nameHtml
        + '<span class="score-time">' + s.time + '</span>'
        + '<span class="score-value">' + s.score + '</span>'
        + '<span class="score-unit">pts</span>'
        + '</div>';
    }).join('');
  }

  // Best: top 5 sorted by score desc
  const best = scores.slice().sort((a, b) => b.score - a.score).slice(0, 5);
  const bestEl = document.getElementById('best-scores');
  if (best.length === 0) {
    bestEl.innerHTML = '<div class="empty">En attente des scores...</div>';
  } else {
    const rankClass = ['gold', 'silver', 'bronze', '', ''];
    bestEl.innerHTML = best.map((s, i) => {
      const unclaimed = !s.name;
      const nameHtml = unclaimed
        ? '<span class="score-name anonymous">???</span>'
        : '<span class="score-name">' + esc(s.name) + '</span>';
      const avatarHtml = (s.has_photo && laptopIp)
        ? '<img class="score-avatar" src="http://' + laptopIp + ':8000/api/photos/' + s.id + '">'
        : '';
      return '<div class="score-entry' + (unclaimed ? ' unclaimed' : '') + '"'
        + (unclaimed ? ' onclick="openClaim(' + s.id + ',' + s.score + ')"' : '') + '>'
        + '<span class="score-rank ' + rankClass[i] + '">#' + (i + 1) + '</span>'
        + avatarHtml
        + nameHtml
        + '<span class="score-time">' + s.time + '</span>'
        + '<span class="score-value' + (i === 0 ? ' best' : '') + '">' + s.score + '</span>'
        + '<span class="score-unit">pts</span>'
        + '</div>';
    }).join('');
  }

  // Stats summary
  if (scores.length > 0) {
    const maxScore = Math.max(...scores.map(s => s.score));
    const avg = Math.round(scores.reduce((a, s) => a + s.score, 0) / scores.length);
    document.getElementById('stat-best').textContent = maxScore;
    document.getElementById('stat-count').textContent = scores.length;
    document.getElementById('stat-avg').textContent = avg;
  }
}

function esc(str) {
  const d = document.createElement('div');
  d.textContent = str;
  return d.innerHTML;
}
```

**Step 4: Add claim modal JavaScript**

Add after the `esc` function:

```javascript
function openClaim(id, score) {
  claimingId = id;
  claimPhotoFile = null;
  document.getElementById('claim-score-display').textContent = score + ' pts';
  document.getElementById('claim-name').value = '';
  document.getElementById('claim-save-btn').disabled = true;
  document.getElementById('claim-photo-preview').style.display = 'none';
  document.getElementById('claim-photo-input').value = '';

  // Show photo row only if laptop is connected
  const photoRow = document.getElementById('claim-photo-row');
  photoRow.classList.toggle('visible', !!laptopIp);

  document.getElementById('claim-overlay').classList.add('active');
  document.getElementById('claim-name').focus();
}

function closeClaim() {
  document.getElementById('claim-overlay').classList.remove('active');
  claimingId = null;
  claimPhotoFile = null;
}

document.getElementById('claim-name').addEventListener('input', function() {
  document.getElementById('claim-save-btn').disabled = !this.value.trim();
});

function previewClaimPhoto(input) {
  if (input.files && input.files[0]) {
    claimPhotoFile = input.files[0];
    const preview = document.getElementById('claim-photo-preview');
    preview.src = URL.createObjectURL(claimPhotoFile);
    preview.style.display = 'block';
  }
}

async function saveClaim() {
  const name = document.getElementById('claim-name').value.trim();
  if (!name || claimingId === null) return;

  const btn = document.getElementById('claim-save-btn');
  btn.disabled = true;
  btn.textContent = '...';

  try {
    // 1. Claim the score on ESP32
    const res = await fetch('/api/claim', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: claimingId, name: name })
    });

    if (!res.ok) {
      const err = await res.json();
      alert(err.error || 'Erreur');
      return;
    }

    // 2. Upload photo to laptop if available
    if (claimPhotoFile && laptopIp) {
      const formData = new FormData();
      formData.append('photo', claimPhotoFile);
      try {
        await fetch('http://' + laptopIp + ':8000/api/photos/' + claimingId, {
          method: 'POST',
          body: formData
        });
      } catch (e) {
        console.warn('Photo upload failed:', e);
      }
    }

    closeClaim();
    fetchScores(); // refresh immediately
  } catch (e) {
    alert('Erreur de connexion');
  } finally {
    btn.disabled = false;
    btn.textContent = 'Valider';
  }
}
```

**Step 5: Update fetchScores to capture laptop_ip**

```javascript
async function fetchScores() {
  try {
    const res = await fetch('/api/scores');
    const data = await res.json();
    allScores = data.scores || [];
    laptopIp = data.laptop_ip || null;
    renderScores(allScores);
    if (document.getElementById('stats-panel').classList.contains('active')) {
      drawChart();
    }
  } catch (e) {
    // silently retry on next poll
  }
}
```

**Step 6: Build, flash, and test in browser**

Run: `idf.py build && idf.py flash monitor`
Connect phone to BAPTHIT WiFi, open captive portal.
Inject scores via serial: `SCORE:750`, `SCORE:800`
- Verify unclaimed scores show "???" and are tappable
- Tap one, enter a name, hit "Valider"
- Verify the score now shows the name
- Verify the modal's photo button is hidden (no laptop registered)

**Step 7: Commit**

```bash
git add main/index.html
git commit -m "feat: add claim modal UI with name input and photo support"
```

**Acceptance Criteria:**

- [ ] **browser**: Unclaimed scores display "???" and have a pointer cursor
      Expected: Visual difference between claimed and unclaimed scores
- [ ] **browser**: Tapping an unclaimed score opens the claim modal with the score value
      Expected: Modal shows correct score, name input is focused
- [ ] **browser**: Entering a name and hitting "Valider" claims the score, modal closes, name appears
      Expected: Score entry updates from "???" to the entered name
- [ ] **browser**: Photo button is hidden when no laptop is registered
      Expected: `claim-photo-row` not visible
- [ ] **browser**: Already-claimed scores are not tappable
      Expected: No click handler, no pointer cursor on claimed scores

---

### Task 7: Web UI — Backend Status in Settings

**Files:**
- Modify: `main/index.html` (settings panel HTML + JS)

**Step 1: Add backend status HTML**

In the settings sheet, add after the OTA progress div and before the closing `</div>` of `.settings-sheet`:

```html
<div class="section-label" style="margin-top:1.5rem">Backend</div>
<div id="backend-status" style="display:flex;align-items:center;gap:0.6rem;padding:0.8rem 1rem;background:var(--bg);border:1px solid var(--border);border-radius:8px;">
  <span id="backend-dot" style="width:8px;height:8px;border-radius:50%;background:var(--text-muted);flex-shrink:0;"></span>
  <span id="backend-text" style="font-size:0.85rem;color:var(--text-dim);">Non connecte</span>
</div>
```

**Step 2: Add backend status update logic**

In the `fetchScores` function, after setting `laptopIp`, add:

```javascript
// Update backend status in settings
const dot = document.getElementById('backend-dot');
const txt = document.getElementById('backend-text');
if (laptopIp) {
  dot.style.background = '#2ecc40';
  dot.style.boxShadow = '0 0 6px #2ecc40';
  txt.style.color = 'var(--text)';
  txt.textContent = 'Connecte (' + laptopIp + ')';
} else {
  dot.style.background = 'var(--text-muted)';
  dot.style.boxShadow = 'none';
  txt.style.color = 'var(--text-dim)';
  txt.textContent = 'Non connecte';
}
```

**Step 3: Build, flash, and test**

Open settings panel in the browser.
- Without laptop: should show "Non connecte" with grey dot.
- Register a backend via curl, then wait for next poll (3s): should show "Connecte (192.168.4.x)" with green dot.

**Step 4: Commit**

```bash
git add main/index.html
git commit -m "feat: show backend connection status in settings panel"
```

**Acceptance Criteria:**

- [ ] **browser**: Open settings, backend shows "Non connecte" with grey dot when no laptop
      Expected: Grey dot, dimmed text
- [ ] **browser + curl**: Register a backend, wait 3s, open settings, see "Connecte (192.168.4.x)" with green dot
      Expected: Green glowing dot, IP address shown

---

### Task 8: Laptop Server — Core Setup

**Files:**
- Create: `backend/server.py`
- Create: `backend/requirements.txt`

**Step 1: Create requirements.txt**

```
fastapi==0.115.*
uvicorn[standard]==0.34.*
httpx==0.28.*
```

**Step 2: Create server.py**

```python
import sqlite3
import httpx
from pathlib import Path
from contextlib import asynccontextmanager

from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel

ESP32_IP = "192.168.4.1"
PHOTOS_DIR = Path(__file__).parent / "photos"
DB_PATH = Path(__file__).parent / "bapthit.db"


def get_db():
    conn = sqlite3.connect(str(DB_PATH))
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS scores (
            id INTEGER PRIMARY KEY,
            hour INTEGER NOT NULL,
            minute INTEGER NOT NULL,
            score INTEGER NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            has_photo INTEGER NOT NULL DEFAULT 0
        )
    """)
    conn.commit()
    conn.close()


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


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    PHOTOS_DIR.mkdir(exist_ok=True)

    # Detect our IP on the BAPTHIT network
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ESP32_IP, 80))
        my_ip = s.getsockname()[0]
    except Exception:
        my_ip = "127.0.0.1"
    finally:
        s.close()

    await register_with_esp32(my_ip)
    yield


app = FastAPI(lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# --- Models ---

class ScoreIn(BaseModel):
    id: int
    hour: int
    minute: int
    score: int


class ClaimIn(BaseModel):
    id: int
    name: str


# --- Endpoints ---

@app.post("/api/scores")
def receive_score(data: ScoreIn):
    """ESP32 pushes new scores here."""
    conn = get_db()
    conn.execute(
        "INSERT OR IGNORE INTO scores (id, hour, minute, score) VALUES (?, ?, ?, ?)",
        (data.id, data.hour, data.minute, data.score),
    )
    conn.commit()
    conn.close()
    return {"ok": True}


@app.post("/api/scores/claim")
def receive_claim(data: ClaimIn):
    """ESP32 forwards claims here."""
    conn = get_db()
    conn.execute(
        "UPDATE scores SET name = ? WHERE id = ?",
        (data.name, data.id),
    )
    conn.commit()
    conn.close()
    return {"ok": True}


@app.get("/api/scores/history")
def get_history():
    """ESP32 fetches all persisted scores on boot."""
    conn = get_db()
    rows = conn.execute(
        "SELECT id, hour, minute, score, name, has_photo FROM scores ORDER BY id"
    ).fetchall()
    conn.close()
    return [
        {
            "id": r["id"],
            "hour": r["hour"],
            "minute": r["minute"],
            "score": r["score"],
            "name": r["name"],
            "has_photo": bool(r["has_photo"]),
        }
        for r in rows
    ]


@app.post("/api/photos/{score_id}")
async def upload_photo(score_id: int, photo: UploadFile = File(...)):
    """Phone uploads photo directly here."""
    # Save photo
    photo_path = PHOTOS_DIR / f"{score_id}.jpg"
    contents = await photo.read()
    photo_path.write_bytes(contents)

    # Update DB
    conn = get_db()
    conn.execute("UPDATE scores SET has_photo = 1 WHERE id = ?", (score_id,))
    conn.commit()
    conn.close()

    # Notify ESP32
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            await client.post(
                f"http://{ESP32_IP}/api/photo_ok",
                json={"id": score_id},
            )
    except Exception:
        pass  # ESP32 will pick it up on next history fetch

    return {"ok": True}


@app.get("/api/photos/{score_id}")
def get_photo(score_id: int):
    """Phone fetches photo for display."""
    photo_path = PHOTOS_DIR / f"{score_id}.jpg"
    if not photo_path.exists():
        raise HTTPException(status_code=404, detail="Photo not found")
    return FileResponse(photo_path, media_type="image/jpeg")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
```

**Step 3: Test the server locally**

```bash
cd backend
pip install -r requirements.txt
python server.py &

# Test score endpoint
curl -X POST http://localhost:8000/api/scores -H 'Content-Type: application/json' -d '{"id":1,"hour":14,"minute":32,"score":750}'
# Expected: {"ok":true}

# Test claim
curl -X POST http://localhost:8000/api/scores/claim -H 'Content-Type: application/json' -d '{"id":1,"name":"Lucas"}'
# Expected: {"ok":true}

# Test history
curl http://localhost:8000/api/scores/history
# Expected: [{"id":1,"hour":14,"minute":32,"score":750,"name":"Lucas","has_photo":false}]

# Test photo upload
curl -X POST http://localhost:8000/api/photos/1 -F 'photo=@test.jpg'
# Expected: {"ok":true}

# Test photo fetch
curl -I http://localhost:8000/api/photos/1
# Expected: 200 OK, content-type: image/jpeg
```

**Step 4: Commit**

```bash
git add backend/
git commit -m "feat: add laptop backend server with FastAPI, SQLite, and photo storage"
```

**Acceptance Criteria:**

- [ ] **curl**: `POST /api/scores` persists a score, `GET /api/scores/history` returns it
      Expected: Score appears in history response with correct fields
- [ ] **curl**: `POST /api/scores/claim` updates the name, visible in history
      Expected: Name field populated in history response
- [ ] **curl**: `POST /api/photos/1` saves a file, `GET /api/photos/1` returns it
      Expected: 200 with image/jpeg content type
- [ ] **curl**: `GET /api/photos/999` returns 404
      Expected: 404 Photo not found
- [ ] **startup**: Server logs "Registered with ESP32 as <ip>" on startup (when ESP32 is reachable)
      Expected: Registration log message visible

---

### Task 9: Integration Test — Full End-to-End Flow

**Files:** None (testing only)

**Step 1: Start the laptop server**

Connect the laptop to the BAPTHIT WiFi network.
```bash
cd backend && python server.py
```
Expected: Server starts, registers with ESP32, ESP32 logs "Backend registered at <ip>".

**Step 2: Verify score forwarding**

Inject a score via serial: `SCORE:850`
```bash
curl http://localhost:8000/api/scores/history
```
Expected: Score appears in the laptop's history.

**Step 3: Verify claiming and photo flow**

From a phone on the BAPTHIT WiFi:
1. Open the captive portal
2. See the score with "???"
3. Tap it, enter a name, take a photo, hit "Valider"
4. Verify the score shows the name and photo thumbnail

From the laptop:
```bash
curl http://localhost:8000/api/scores/history
ls backend/photos/
```
Expected: Score has name and has_photo=true. Photo file exists.

**Step 4: Verify boot recovery**

Reboot the ESP32 (power cycle or `esp_restart()`).
The laptop server should re-register, and the ESP32 should fetch history.
```bash
curl http://192.168.4.1/api/scores
```
Expected: All previously persisted scores appear with names and photo flags.

**Step 5: Verify degraded mode (no laptop)**

Stop the laptop server. Reboot the ESP32.
Inject a score: `SCORE:600`
From a phone: open captive portal, tap score, enter name.
Expected: Everything works except photos (no photo button visible, no persistence).

**Acceptance Criteria:**

- [ ] **end-to-end**: Score injected on ESP32 appears in laptop's SQLite database
      Expected: `curl /api/scores/history` shows the score
- [ ] **end-to-end**: Claim from phone reaches both ESP32 (in-memory) and laptop (SQLite)
      Expected: Both `curl 192.168.4.1/api/scores` and `curl localhost:8000/api/scores/history` show the name
- [ ] **end-to-end**: Photo uploaded from phone is stored on laptop, thumbnail visible in UI
      Expected: Photo file in `backend/photos/`, thumbnail renders in browser
- [ ] **end-to-end**: After ESP32 reboot with laptop present, scores are restored
      Expected: All scores, names, and photo flags survive the reboot
- [ ] **degraded**: Without laptop, scores and claiming work, photo button is hidden
      Expected: Full functionality minus photos and persistence

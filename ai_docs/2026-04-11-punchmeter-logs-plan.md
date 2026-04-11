# PunchMeter Logs Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add PunchMeter state-machine instrumentation and a full-screen in-app log viewer reachable from Settings.

**Architecture:** Additive callback hook in the submodule (no ESP-IDF headers pulled in), ring buffer + `GET /api/logs` in `main.cpp`, full-screen Logs overlay in `index.html` with 500 ms polling and since-based pagination.

**Tech Stack:** ESP-IDF v6.0, Arduino-as-component, FreeRTOS, esp_http_server, vanilla HTML/JS.

**Reference design:** `ai_docs/2026-04-11-punchmeter-logs-design.md`

---

## Task 1: Add logging callback prototype to `punchmeter.h`

**Files:**
- Modify: `components/punchmeter/src/punchmeter.h`

**Step 1: Add the typedef + prototype**

Insert right before `#endif // PUNCHMETER_H`:

```cpp
// Optional logging hook. The host can register a callback to receive
// state-machine and scoring messages; standalone Arduino builds that
// never call this stay zero-overhead because the internal helper
// short-circuits when the pointer is NULL.
typedef void (*punchmeter_log_fn)(const char *msg);
void punchmeter_set_logger(punchmeter_log_fn fn);
```

**Acceptance Criteria:**
- [ ] Read `components/punchmeter/src/punchmeter.h` — typedef and prototype are present inside the header guard.

---

## Task 2: Implement `punchmeter_set_logger` + `pm_log` helper

**Files:**
- Modify: `components/punchmeter/src/punchmeter.cpp`

**Step 1: Add `#include <stdarg.h>` if not already present**

At the top of `punchmeter.cpp`, verify `#include <Arduino.h>` is there; add `#include <stdarg.h>` below it if absent.

**Step 2: Add static pointer + helper before the `// ---------- state ----------` block**

Insert:

```cpp
// ---------- logging hook ----------

static punchmeter_log_fn pm_logger = NULL;

void punchmeter_set_logger(punchmeter_log_fn fn) {
    pm_logger = fn;
}

static void pm_log(const char *fmt, ...) {
    if (!pm_logger) return;
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pm_logger(buf);
}
```

**Acceptance Criteria:**
- [ ] Read `components/punchmeter/src/punchmeter.cpp` — `pm_logger` static and `pm_log` helper are both defined.

---

## Task 3: Sprinkle log calls in the state machine

**Files:**
- Modify: `components/punchmeter/src/punchmeter.cpp` — `punchmeter_loop()` (the switch)

**Step 1: IDLE entry log**

Replace the body of `case IDLE:` (currently `clearDisplay(); if (getBagPos() == DOWN) state = READY;`) with:

```cpp
case IDLE:
    if (state_logged != IDLE) { pm_log("-> IDLE"); state_logged = IDLE; }
    clearDisplay();
    if (getBagPos() == DOWN) {
        state = READY;
    }
    break;
```

**Step 2: Add `state_logged` tracking static**

In the `// ---------- state ----------` block, alongside `static States state = IDLE;`, add:

```cpp
static States state_logged = (States)-1;
```

(Cast is fine: ensures the first call logs the initial state.)

**Step 3: READY entry log**

Add `if (state_logged != READY) { pm_log("-> READY"); state_logged = READY; }` as the first line of `case READY:` body.

**Step 4: MEAS entry log**

Add the same pattern in `case MEAS:`:

```cpp
case MEAS:
    if (state_logged != MEAS) { pm_log("-> MEAS start=%lu", start); state_logged = MEAS; }
    if (getBagPos() == UNKNOWN) {
```

Note: `start` is set when transitioning from READY → MEAS (in the READY case). The log prints the value the first time we iterate the MEAS case, which is right after the transition.

**Step 5: Score computation log**

In `case MEAS:` → `else if (getBagPos() == UP)` block, right after
```cpp
    score = micros() - start;
```
capture the raw value and log it alongside the final:

```cpp
    unsigned long score_raw = score;
    if (score < maxScore) {
        last_score = 999;
        rollScore(999);
    } else {
        score = 999 - (999 * (score - maxScore)) / (minScore - maxScore);
        last_score = (int)score;
        rollScore(score);
    }
    pm_log("score raw=%luus final=%d", score_raw, last_score);
```

**Step 6: DISP entry log**

Add to `case DISP:` first line:

```cpp
case DISP:
    if (state_logged != DISP) {
        pm_log("-> DISP lastBlink=%lu now=%lu", lastBlinkDate, millis());
        state_logged = DISP;
    }
```

**Step 7: Log config updates**

In `punchmeter_set_config`, after the assignments:

```cpp
void punchmeter_set_config(const PunchmeterConfig *cfg) {
    if (cfg) {
        maxScore          = cfg->maxScore;
        // ... existing assignments ...
        waveDelay         = cfg->waveDelay;
        pm_log("config: max=%lu min=%lu rollDelay=%d incr=%d",
               maxScore, minScore, defaultRollDelay, defaultIncrement);
    }
}
```

**Step 8: Commit inside the submodule**

```bash
cd components/punchmeter
git add src/punchmeter.h src/punchmeter.cpp
git commit -m "Add optional logging hook with state-machine instrumentation"
cd ../..
```

**Acceptance Criteria:**
- [ ] Run `cd components/punchmeter && git diff HEAD~1 HEAD -- src/punchmeter.cpp | grep -c "pm_log("`. Expected: 6 or more (6 call sites).
- [ ] Run `cd components/punchmeter && git log -1 --stat`. Expected: commit touches `punchmeter.h` and `punchmeter.cpp`.

---

## Task 4: Expose prototype in `main/punchmeter_api.h`

**Files:**
- Modify: `main/punchmeter_api.h`

**Step 1: Add typedef + prototype**

Inside the header guard, after the existing `void punchmeter_set_config(...)`, add:

```cpp
typedef void (*punchmeter_log_fn)(const char *msg);
void punchmeter_set_logger(punchmeter_log_fn fn);
```

**Acceptance Criteria:**
- [ ] Read `main/punchmeter_api.h` — both declarations are present.

---

## Task 5: Add ring buffer + `pm_log_sink` in `main.cpp`

**Files:**
- Modify: `main/main.cpp`

**Step 1: Add constants and buffer below the existing `#define MAX_NAME_LEN 32`**

```cpp
// ---------------------------------------------------------------------------
// Punchmeter log ring buffer
// ---------------------------------------------------------------------------

#define LOG_BUF_SIZE 64
#define LOG_MSG_LEN  120

typedef struct {
    uint32_t seq;      // monotonic, 0 = empty slot
    uint32_t ts_ms;
    char     msg[LOG_MSG_LEN];
} log_entry_t;

static log_entry_t log_buf[LOG_BUF_SIZE];
static int         log_head = 0;
static uint32_t    log_next_seq = 1;
static SemaphoreHandle_t log_mutex = NULL;

static void pm_log_sink(const char *msg)
{
    if (!log_mutex) return;
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_entry_t *e = &log_buf[log_head];
    e->seq = log_next_seq++;
    e->ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int j = 0;
    for (int i = 0; msg[i] && j < LOG_MSG_LEN - 1; i++) {
        char c = msg[i];
        if (c != '"' && c != '\\' && c >= 0x20) e->msg[j++] = c;
    }
    e->msg[j] = '\0';
    log_head = (log_head + 1) % LOG_BUF_SIZE;
    xSemaphoreGive(log_mutex);
    ESP_LOGI("PM", "%s", msg);
}
```

**Note on placement:** this block must sit before `add_score` (so the sink is visible to `app_main` / the webserver). Insert it right after the `static char laptop_ip[16] = "";` line and before the `// Wall-clock sync` section added in the previous turn.

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `log_buf`, `log_mutex`, and `pm_log_sink` are declared.

---

## Task 6: Create `log_mutex` and register the sink in `app_main`

**Files:**
- Modify: `main/main.cpp` — `app_main()`

**Step 1: Create the mutex and register the logger**

In `app_main`, just after `config_mutex = xSemaphoreCreateMutex();` add:

```cpp
log_mutex = xSemaphoreCreateMutex();
punchmeter_set_logger(pm_log_sink);
```

**Note:** order matters — `log_mutex` must exist before the punchmeter task (which may call `pm_log_sink`) starts. Since `punchmeter_task` is created a few lines below, this placement is safe.

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `log_mutex = xSemaphoreCreateMutex()` and `punchmeter_set_logger(pm_log_sink)` are called in `app_main` before `xTaskCreatePinnedToCore(punchmeter_task, ...)`.

---

## Task 7: Add `GET /api/logs` handler

**Files:**
- Modify: `main/main.cpp`

**Step 1: Insert the handler just above `config_get_handler`**

```cpp
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    uint32_t since = 0;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 64) {
        char query[64];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
                since = strtoul(val, NULL, 10);
            }
        }
    }

    char *buf = (char *)malloc(12288);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = sprintf(buf, "{\"logs\":[");
    bool first = true;

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    // Iterate from oldest to newest: start after log_head (which points to the
    // next slot to overwrite = the oldest entry in a full buffer).
    for (int i = 0; i < LOG_BUF_SIZE; i++) {
        int idx = (log_head + i) % LOG_BUF_SIZE;
        log_entry_t *e = &log_buf[idx];
        if (e->seq == 0 || e->seq <= since) continue;
        if (pos > 11800) break;  // safety margin
        if (!first) buf[pos++] = ',';
        first = false;
        pos += sprintf(buf + pos,
            "{\"s\":%u,\"t\":%u,\"m\":\"%s\"}",
            (unsigned)e->seq, (unsigned)e->ts_ms, e->msg);
    }
    xSemaphoreGive(log_mutex);

    pos += sprintf(buf + pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}
```

**Step 2: Register the URI in `start_webserver`, after `config_get_uri`, before `captive_uri`**

```cpp
const httpd_uri_t logs_get_uri = {
    .uri = "/api/logs",
    .method = HTTP_GET,
    .handler = logs_get_handler,
    .user_ctx = NULL,
};
httpd_register_uri_handler(server, &logs_get_uri);
```

**Acceptance Criteria:**
- [ ] Read `main/main.cpp` — `logs_get_handler` exists and `logs_get_uri` is registered before `captive_uri`.

---

## Task 8: Commit firmware + rebuild

**Files:**
- Modified: `main/main.cpp`, `main/punchmeter_api.h`

**Step 1: Commit**

```bash
git add main/main.cpp main/punchmeter_api.h
git commit -m "Add pm_log_sink ring buffer and GET /api/logs endpoint"
```

**Step 2: Build**

```bash
bash -c '. /home/llenoir/.espressif/v6.0/esp-idf/export.sh >/dev/null 2>&1 && idf.py build' 2>&1 | tail -20
```

Expected: `Project build complete.`

**Acceptance Criteria:**
- [ ] Run the build command above. Expected: ends with `Project build complete.` and a fresh `build/bapthit.bin`.
- [ ] Run `ls -la build/bapthit.bin`. Expected: mtime ≈ now.

---

## Task 9: Add CSS for the Logs overlay

**Files:**
- Modify: `main/index.html`

**Step 1: Insert after the `/* ---- CONFIG FORM ---- */` block, still inside `<style>`**

```css
/* ---- LOGS OVERLAY ---- */

.logs-overlay {
  display: none;
  position: fixed;
  inset: 0;
  background: #050505;
  z-index: 100;
  flex-direction: column;
  animation: fadeIn 0.2s ease;
}
.logs-overlay.active { display: flex; }

.logs-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1rem 1.2rem;
  border-bottom: 1px solid var(--border);
  background: var(--surface);
}
.logs-title {
  font-family: Impact, 'Arial Black', sans-serif;
  font-size: 1.1rem;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  color: var(--red);
}
.logs-close {
  width: 32px;
  height: 32px;
  background: none;
  border: 1px solid var(--border);
  border-radius: 50%;
  color: var(--text-dim);
  font-size: 1.1rem;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
}
.logs-close:hover { border-color: var(--red-dim); color: var(--red); }

.logs-body {
  flex: 1;
  overflow-y: auto;
  padding: 0.8rem 1rem;
  font-family: 'Courier New', 'Consolas', monospace;
  font-size: 0.78rem;
  line-height: 1.5;
  color: var(--text);
  white-space: pre-wrap;
  word-break: break-word;
}
.logs-body .log-line {
  display: flex;
  gap: 0.6rem;
  padding: 0.1rem 0;
}
.logs-body .log-ts {
  color: var(--text-muted);
  flex-shrink: 0;
  width: 5rem;
}
.logs-body .log-msg { color: var(--text); flex: 1; }

.logs-body .empty {
  color: var(--text-muted);
  text-align: center;
  padding: 2rem 0;
}

.logs-footer {
  display: flex;
  gap: 0.6rem;
  padding: 0.8rem 1rem;
  border-top: 1px solid var(--border);
  background: var(--surface);
}
.logs-btn {
  flex: 1;
  padding: 0.7rem;
  background: var(--surface-alt);
  border: 1px solid var(--border);
  color: var(--text-dim);
  border-radius: 8px;
  font-size: 0.75rem;
  font-weight: 700;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  cursor: pointer;
}
.logs-btn:hover { border-color: var(--red-dim); color: var(--text); }
.logs-btn.paused { color: var(--red); border-color: var(--red-dim); }

.logs-open-btn {
  width: 100%;
  margin-top: 0.8rem;
  padding: 0.7rem;
  background: var(--surface-alt);
  border: 1px solid var(--border);
  color: var(--text);
  border-radius: 8px;
  font-size: 0.75rem;
  font-weight: 700;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  cursor: pointer;
}
.logs-open-btn:hover { border-color: var(--red-dim); color: var(--red); }
```

**Acceptance Criteria:**
- [ ] Read `main/index.html` — the new CSS block is inside `<style>`.

---

## Task 10: Add Logs button in Settings + overlay markup

**Files:**
- Modify: `main/index.html`

**Step 1: Add the Logs button inside Settings sheet**

Right after the `<div class="config-form" id="config-form"> ... </div>` block (still inside `.settings-sheet`), add:

```html
<div class="section-label" style="margin-top:1.5rem">Debug</div>
<button class="logs-open-btn" onclick="openLogs()">Ouvrir les logs</button>
```

**Step 2: Add the overlay markup just before the `<script>` tag**

```html
<div class="logs-overlay" id="logs-overlay">
  <div class="logs-header">
    <span class="logs-title">Logs PunchMeter</span>
    <button class="logs-close" onclick="closeLogs()">&times;</button>
  </div>
  <div class="logs-body" id="logs-body">
    <div class="empty">En attente de logs...</div>
  </div>
  <div class="logs-footer">
    <button class="logs-btn" onclick="clearLogs()">Clear</button>
    <button class="logs-btn" id="logs-pause-btn" onclick="togglePauseLogs()">Pause</button>
  </div>
</div>
```

**Acceptance Criteria:**
- [ ] Read `main/index.html` — the "Ouvrir les logs" button is inside the settings sheet and the `logs-overlay` div is present.

---

## Task 11: Add Logs JS (open/close, poll, render, clear, pause)

**Files:**
- Modify: `main/index.html`

**Step 1: Add the Logs module after `saveConfig`**

```javascript
// ---- Logs overlay ----

let logsState = {
  lastSeq: 0,
  paused: false,
  interval: null,
  bodyEl: null,
  tsRef: null,
  stuckBottom: true,
};

function openLogs() {
  const overlay = document.getElementById('logs-overlay');
  overlay.classList.add('active');
  logsState.bodyEl = document.getElementById('logs-body');
  logsState.bodyEl.addEventListener('scroll', onLogsScroll);
  if (logsState.interval) clearInterval(logsState.interval);
  logsState.interval = setInterval(pollLogs, 500);
  pollLogs();
}

function closeLogs() {
  document.getElementById('logs-overlay').classList.remove('active');
  if (logsState.interval) {
    clearInterval(logsState.interval);
    logsState.interval = null;
  }
  if (logsState.bodyEl) {
    logsState.bodyEl.removeEventListener('scroll', onLogsScroll);
  }
}

function onLogsScroll() {
  const el = logsState.bodyEl;
  if (!el) return;
  const gap = el.scrollHeight - el.scrollTop - el.clientHeight;
  logsState.stuckBottom = gap < 50;
}

function togglePauseLogs() {
  logsState.paused = !logsState.paused;
  const btn = document.getElementById('logs-pause-btn');
  btn.textContent = logsState.paused ? 'Resume' : 'Pause';
  btn.classList.toggle('paused', logsState.paused);
}

function clearLogs() {
  const body = logsState.bodyEl || document.getElementById('logs-body');
  body.innerHTML = '<div class="empty">(vide)</div>';
}

function fmtLogTs(ts_ms) {
  if (logsState.tsRef === null) logsState.tsRef = ts_ms;
  const rel = ts_ms - logsState.tsRef;
  const s = Math.floor(rel / 1000);
  const ms = rel % 1000;
  return s + '.' + String(ms).padStart(3, '0');
}

async function pollLogs() {
  if (logsState.paused) return;
  try {
    const res = await fetch('/api/logs?since=' + logsState.lastSeq);
    if (!res.ok) return;
    const data = await res.json();
    if (!data.logs || data.logs.length === 0) return;

    const body = logsState.bodyEl;
    if (body.querySelector('.empty')) body.innerHTML = '';

    const frag = document.createDocumentFragment();
    for (const entry of data.logs) {
      if (entry.s > logsState.lastSeq) logsState.lastSeq = entry.s;
      const line = document.createElement('div');
      line.className = 'log-line';
      const ts = document.createElement('span');
      ts.className = 'log-ts';
      ts.textContent = '[' + fmtLogTs(entry.t) + ']';
      const msg = document.createElement('span');
      msg.className = 'log-msg';
      msg.textContent = entry.m;
      line.appendChild(ts);
      line.appendChild(msg);
      frag.appendChild(line);
    }
    body.appendChild(frag);

    if (logsState.stuckBottom) {
      body.scrollTop = body.scrollHeight;
    }
  } catch (e) {
    // silent retry on next tick
  }
}
```

**Acceptance Criteria:**
- [ ] Read `main/index.html` — `openLogs`, `closeLogs`, `pollLogs`, `clearLogs`, `togglePauseLogs`, `fmtLogTs`, `onLogsScroll` are defined.

---

## Task 12: Commit frontend + rebuild firmware

**Step 1: Commit**

```bash
git add main/index.html
git commit -m "Add full-screen PunchMeter logs overlay with polling"
```

**Step 2: Rebuild**

```bash
bash -c '. /home/llenoir/.espressif/v6.0/esp-idf/export.sh >/dev/null 2>&1 && idf.py build' 2>&1 | tail -15
```

Expected: `Project build complete.` + `[N/M] Building ASM object esp-idf/main/.../index.html.S.obj` in the log (confirms the embedded HTML was regenerated).

**Acceptance Criteria:**
- [ ] Run the build. Expected: `Project build complete.` with no errors.
- [ ] Run `ls -la build/bapthit.bin`. Expected: fresh mtime.

---

## Task 13: Update submodule pointer in parent repo

**Step 1: Verify the submodule has new commits**

```bash
git status
git submodule status
```

Expected: `modified: components/punchmeter (new commits)` and a `+` prefix on the submodule SHA.

**Step 2: Commit the pointer bump**

```bash
git add components/punchmeter
git commit -m "Bump punchmeter: add optional logging hook"
```

**Acceptance Criteria:**
- [ ] Run `git submodule status`. Expected: no `+` prefix.
- [ ] Run `git log -1 --stat`. Expected: commit touches `components/punchmeter` only.

---

## Final smoke test (after flash)

- [ ] Flash the new binary (OTA or `make flash`).
- [ ] Curl the logs endpoint before doing anything: `curl http://192.168.4.1/api/logs?since=0`. Expected: `{"logs":[{"s":1,...,"m":"-> IDLE"}, ...]}` with at least the initial state entries.
- [ ] Open the phone page → Settings → Ouvrir les logs. Expected: overlay full-screen with the same initial entries visible.
- [ ] Trigger a score: `make serial-score SCORE=500`. Expected: new lines appear within ~500 ms — something like `score raw=...us final=500` and `-> DISP lastBlink=X now=Y`.
- [ ] Observe the `-> DISP lastBlink=X now=Y` line: if `Y >= X` (it will be), that confirms the always-false blink condition hypothesis.
- [ ] Tap Pause, verify no new lines appear. Tap Resume, verify polling resumes.
- [ ] Tap Clear, verify the on-screen list empties. Trigger another score, verify new entries still come in (ESP32 buffer untouched).
- [ ] Close overlay, verify polling stops (no `/api/logs` calls in network tab).

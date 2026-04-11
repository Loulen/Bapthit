#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "punchmeter_api.h"
#include "wifi_sta.h"
#include "score_queue.h"
#include "ws_client.h"

static const char *TAG = "bapthit";

// ---------------------------------------------------------------------------
// Shared PunchMeter config protected by mutex
// ---------------------------------------------------------------------------

typedef struct {
    PunchmeterConfig cfg;
    bool             dirty;
    SemaphoreHandle_t mux;
} shared_cfg_t;

static shared_cfg_t s_cfg = {
    .cfg = {
        .maxScore         = 20000,
        .minScore         = 100000,
        .defaultRollDelay = 15,
        .rollDelayMod     = 1,
        .rollThresh       = 60,
        .slowRollThresh   = 7,
        .slowRollDelayMod = 100,
        .defaultIncrement = 15,
        .blinkDelay       = 600,
        .waveDuration     = 1500,
        .waveDelay        = 200,
    },
    .dirty = true,  // apply defaults once at boot
    .mux = NULL,
};

static int      s_next_score_id = 1;
static uint32_t s_boot_id = 0;  // randomized once at boot, sent to backend

// ---------------------------------------------------------------------------
// Punchmeter log sink — forward to ESP_LOG and stream to backend over WS
// ---------------------------------------------------------------------------

// JSON-escape a single message into the destination buffer. We strip the
// few characters that would break JSON (quotes, backslashes, controls)
// rather than escaping them — the PunchMeter log format is plain ASCII.
static int sanitize_log_msg(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
        char c = src[i];
        if (c != '"' && c != '\\' && c >= 0x20) {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    return (int)j;
}

static void pm_log_sink(const char *msg)
{
    ESP_LOGI("PM", "%s", msg);
    if (!ws_client_is_connected()) return;
    char clean[160];
    sanitize_log_msg(clean, sizeof(clean), msg);
    char frame[224];
    int n = snprintf(frame, sizeof(frame),
        "{\"type\":\"log\",\"level\":\"I\",\"msg\":\"%s\"}", clean);
    if (n > 0) ws_client_send_text(frame, (size_t)n);
}

// ---------------------------------------------------------------------------
// JSON helpers (tiny parsers; same shape as the legacy main.cpp)
// ---------------------------------------------------------------------------

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
    *out = strtoul(colon + 1, NULL, 10);
    return true;
}

// ---------------------------------------------------------------------------
// WS message handling
// ---------------------------------------------------------------------------

static void apply_config_from_json(const char *buf)
{
    xSemaphoreTake(s_cfg.mux, portMAX_DELAY);
    unsigned long ul_val;
    int int_val;
    if (json_parse_ulong(buf, "maxScore",         &ul_val))  s_cfg.cfg.maxScore         = ul_val;
    if (json_parse_ulong(buf, "minScore",         &ul_val))  s_cfg.cfg.minScore         = ul_val;
    if (json_parse_int  (buf, "defaultRollDelay", &int_val)) s_cfg.cfg.defaultRollDelay = int_val;
    if (json_parse_int  (buf, "rollDelayMod",     &int_val)) s_cfg.cfg.rollDelayMod     = int_val;
    if (json_parse_int  (buf, "rollThresh",       &int_val)) s_cfg.cfg.rollThresh       = int_val;
    if (json_parse_int  (buf, "slowRollThresh",   &int_val)) s_cfg.cfg.slowRollThresh   = int_val;
    if (json_parse_int  (buf, "slowRollDelayMod", &int_val)) s_cfg.cfg.slowRollDelayMod = int_val;
    if (json_parse_int  (buf, "defaultIncrement", &int_val)) s_cfg.cfg.defaultIncrement = int_val;
    if (json_parse_int  (buf, "blinkDelay",       &int_val)) s_cfg.cfg.blinkDelay       = int_val;
    if (json_parse_int  (buf, "waveDuration",     &int_val)) s_cfg.cfg.waveDuration     = int_val;
    if (json_parse_int  (buf, "waveDelay",        &int_val)) s_cfg.cfg.waveDelay        = int_val;
    s_cfg.dirty = true;
    xSemaphoreGive(s_cfg.mux);
    ESP_LOGI(TAG, "config received from backend");
}

static void on_ws_text(const char *data, size_t len)
{
    char  stackbuf[512];
    char *buf = stackbuf;
    if (len >= sizeof(stackbuf)) {
        buf = (char *)malloc(len + 1);
        if (!buf) return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    // Route by "type"
    const char *t = strstr(buf, "\"type\"");
    if (t) {
        const char *q1 = strchr(t + 6, '"');
        if (q1) {
            q1++;
            const char *q2 = strchr(q1, '"');
            if (q2) {
                int tl = (int)(q2 - q1);
                if (tl == 6 && strncmp(q1, "config", 6) == 0) {
                    apply_config_from_json(buf);
                } else if (tl == 4 && strncmp(q1, "ping", 4) == 0) {
                    // no-op; native WS keepalive is enough
                } else {
                    ESP_LOGW(TAG, "unknown msg type");
                }
            }
        }
    }

    if (buf != stackbuf) free(buf);
}

static void on_ws_connect(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char msg[128];
    int n = snprintf(msg, sizeof(msg),
        "{\"type\":\"hello\",\"fw\":\"%s\",\"boot\":%u}",
        d ? d->version : "?", (unsigned)s_boot_id);
    ws_client_send_text(msg, (size_t)n);
    ESP_LOGI(TAG, "sent hello (boot=%u)", (unsigned)s_boot_id);
}

// ---------------------------------------------------------------------------
// PunchMeter task (core 1)
// ---------------------------------------------------------------------------

static void punchmeter_task(void *arg)
{
    (void)arg;
    punchmeter_setup();
    int prev_score = -1;

    while (1) {
        if (s_cfg.dirty && xSemaphoreTake(s_cfg.mux, 0) == pdTRUE) {
            PunchmeterConfig local = s_cfg.cfg;
            s_cfg.dirty = false;
            xSemaphoreGive(s_cfg.mux);
            punchmeter_set_config(&local);
        }

        punchmeter_loop();

        int cur = punchmeter_get_last_score();
        if (cur >= 0 && cur != prev_score) {
            score_item_t item = {
                .id = s_next_score_id++,
                .score = cur,
                .ts_epoch = time(NULL),
            };
            score_queue_push(&item);
            prev_score = cur;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// Uploader task — drain queue → WS
// ---------------------------------------------------------------------------

static void uploader_task(void *arg)
{
    (void)arg;
    while (1) {
        score_item_t item;
        if (score_queue_peek(&item)) {
            if (ws_client_is_connected()) {
                char msg[128];
                int n = snprintf(msg, sizeof(msg),
                    "{\"type\":\"score\",\"id\":%d,\"score\":%d,\"ts\":%lld}",
                    item.id, item.score, (long long)item.ts_epoch);
                if (ws_client_send_text(msg, (size_t)n) == ESP_OK) {
                    score_queue_pop();
                } else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Bapthit firmware v%s started (probe mode)",
             app_desc ? app_desc->version : "?");

    // OTA rollback confirmation
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK
        && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA firmware validated");
    }

    // NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Init mDNS so that gethostbyname() can resolve <host>.local
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("bapthit-device");

    // Synchronization primitives
    s_cfg.mux = xSemaphoreCreateMutex();
    score_queue_init();
    punchmeter_set_logger(pm_log_sink);

    // Random boot session id, used by the backend to detect a fresh boot
    // and remap our local score ids (which always start at 1) to a
    // collision-free range in the score table.
    s_boot_id = esp_random();

    // PunchMeter task on core 1 — keeps the game responsive even while
    // WiFi/WS is bringing itself up.
    xTaskCreatePinnedToCore(punchmeter_task, "punchmeter", 4096, NULL, 5, NULL, 1);

    // WiFi STA — block until IP
    ESP_ERROR_CHECK(wifi_sta_start_and_wait());

    // WebSocket client
    ws_client_cfg_t wsc = {
        .host = CONFIG_BAPTHIT_BACKEND_HOST,
        .port = CONFIG_BAPTHIT_BACKEND_PORT,
        .path = "/ws/device",
        .on_connect = on_ws_connect,
        .on_text = on_ws_text,
    };
    ESP_ERROR_CHECK(ws_client_start(&wsc));

    // Uploader task
    xTaskCreate(uploader_task, "uploader", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Running on partition: %s", running->label);
}

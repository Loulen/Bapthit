#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "nvs_flash.h"

static const char *TAG = "bapthit";

#define WIFI_SSID "BAPTHIT"
#define WIFI_CHANNEL 1
#define MAX_STA_CONN 8

// ---------------------------------------------------------------------------
// Score store
// ---------------------------------------------------------------------------

#define MAX_SCORES 64

typedef struct {
    int hour;
    int minute;
    int score;
} score_entry_t;

static score_entry_t scores[MAX_SCORES];
static int score_count = 0;

static void add_score(int hour, int minute, int score)
{
    if (score_count < MAX_SCORES) {
        scores[score_count].hour = hour;
        scores[score_count].minute = minute;
        scores[score_count].score = score;
        score_count++;
    }
}

static int cmp_time(const void *a, const void *b)
{
    const score_entry_t *sa = (const score_entry_t *)a;
    const score_entry_t *sb = (const score_entry_t *)b;
    int ta = sa->hour * 60 + sa->minute;
    int tb = sb->hour * 60 + sb->minute;
    return ta - tb;
}

static void generate_mock_scores(void)
{
    for (int i = 0; i < 15; i++) {
        int hour = 13 + (int)(esp_random() % 10);   // 13h - 22h
        int minute = (int)(esp_random() % 60);
        int score = 400 + (int)(esp_random() % 800); // 400 - 1199
        add_score(hour, minute, score);
    }
    qsort(scores, score_count, sizeof(score_entry_t), cmp_time);
    ESP_LOGI(TAG, "Generated %d mock scores", score_count);
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

static esp_err_t scores_get_handler(httpd_req_t *req)
{
    // Build JSON manually — avoids cJSON dependency
    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = sprintf(buf, "{\"scores\":[");
    for (int i = 0; i < score_count; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += sprintf(buf + pos, "{\"time\":\"%02dh%02d\",\"score\":%d}",
                       scores[i].hour, scores[i].minute, scores[i].score);
    }
    pos += sprintf(buf + pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

// Embedded HTML file
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(server, &root_uri);

    const httpd_uri_t scores_uri = {
        .uri = "/api/scores",
        .method = HTTP_GET,
        .handler = scores_get_handler,
    };
    httpd_register_uri_handler(server, &scores_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return server;
}

// ---------------------------------------------------------------------------
// Wi-Fi AP
// ---------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected, AID=%d", event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station disconnected, AID=%d", event->aid);
    }
}

static void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started — SSID: %s, Channel: %d", WIFI_SSID, WIFI_CHANNEL);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Bapthit firmware v%s started", app_desc->version);

    // Validate OTA firmware if pending verification
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK
        && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New OTA firmware — validating...");
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA firmware validated");
    }

    // Initialize NVS (required by Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking and Wi-Fi AP
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_ap();

    // Generate mock data and start HTTP server
    generate_mock_scores();
    start_webserver();

    ESP_LOGI(TAG, "Running on partition: %s", running->label);
}

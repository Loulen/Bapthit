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
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "punchmeter_api.h"

static const char *TAG = "bapthit";

#define WIFI_SSID "BAPTHIT"
#define WIFI_CHANNEL 1
#define MAX_STA_CONN 8

// ---------------------------------------------------------------------------
// PunchMeter integration — score queue + config
// ---------------------------------------------------------------------------

#define SCORE_QUEUE_SIZE 16

typedef struct {
    int score;
    int64_t timestamp_ms;
} score_event_t;

static QueueHandle_t score_queue = NULL;

// Shared config protected by mutex
typedef struct {
    unsigned long scoreRef;
} shared_config_t;

static shared_config_t shared_config = { .scoreRef = 500000 };
static SemaphoreHandle_t config_mutex = NULL;

// ---------------------------------------------------------------------------
// Score store
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// PunchMeter task (runs on core 1)
// ---------------------------------------------------------------------------

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
            xQueueSend(score_queue, &evt, 0);
            prev_score = current_score;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------

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
    if (!colon) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid id");
        return ESP_FAIL;
    }
    int id = atoi(colon + 1);

    // Parse "name" field
    char *name_key = strstr(buf, "\"name\"");
    if (!name_key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
        return ESP_FAIL;
    }
    char *name_colon = strchr(name_key, ':');
    if (!name_colon) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid name");
        return ESP_FAIL;
    }
    char *quote1 = strchr(name_colon, '"');
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

    // Copy name, sanitizing JSON-breaking characters
    int src_len = quote2 - quote1;
    int dst = 0;
    for (int i = 0; i < src_len && dst < MAX_NAME_LEN - 1; i++) {
        char c = quote1[i];
        if (c != '"' && c != '\\') {
            scores[idx].name[dst++] = c;
        }
    }
    scores[idx].name[dst] = '\0';
    if (dst == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name is empty");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Score %d claimed by '%s'", id, scores[idx].name);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
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

// OTA update handler — receives firmware binary via POST
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA update started, content length: %d", req->content_len);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No update partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: writing to partition '%s' at offset 0x%lx",
             update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received_total = 0;

    while (remaining > 0) {
        int received = httpd_req_recv(req, buf, (remaining < 4096) ? remaining : 4096);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA: receive error");
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        remaining -= received;
        received_total += received;

        if (received_total % (64 * 1024) < 4096) {
            ESP_LOGI(TAG, "OTA progress: %d/%d bytes", received_total, req->content_len);
        }
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting in 1s...");
    httpd_resp_sendstr(req, "OK");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// Captive portal: redirect all unknown URLs to root
// Android checks: /generate_204, /gen_204, /connecttest.txt
// iOS checks: /hotspot-detect.html
// Windows checks: /ncsi.txt, /connecttest.txt
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

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

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &root_uri);

    const httpd_uri_t scores_uri = {
        .uri = "/api/scores",
        .method = HTTP_GET,
        .handler = scores_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &scores_uri);

    const httpd_uri_t ota_uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &ota_uri);

    const httpd_uri_t config_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &config_uri);

    const httpd_uri_t claim_uri = {
        .uri = "/api/claim",
        .method = HTTP_POST,
        .handler = claim_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &claim_uri);

    // Captive portal catch-all: any other URL redirects to /
    const httpd_uri_t captive_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &captive_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return server;
}

// ---------------------------------------------------------------------------
// Serial test input — send "SCORE:750" via serial to inject fake scores
// ---------------------------------------------------------------------------

static void serial_test_task(void *arg)
{
    char line[64];
    int pos = 0;

    ESP_LOGI(TAG, "Serial test input ready — send SCORE:<value> to inject scores");

    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                if (strncmp(line, "SCORE:", 6) == 0) {
                    int val = atoi(line + 6);
                    if (val >= 0 && val <= 999) {
                        score_event_t evt = {
                            .score = val,
                            .timestamp_ms = esp_timer_get_time() / 1000,
                        };
                        xQueueSend(score_queue, &evt, 0);
                        ESP_LOGI(TAG, "Test score injected: %d", val);
                    }
                }
                pos = 0;
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)c;
        }
    }
}

// ---------------------------------------------------------------------------
// Captive portal DNS server — resolves ALL domains to 192.168.4.1
// ---------------------------------------------------------------------------

#define DNS_PORT 53
#define DNS_MAX_LEN 256

static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buf[DNS_MAX_LEN];
    uint8_t tx_buf[DNS_MAX_LEN];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int len = recvfrom(sock, rx_buf, DNS_MAX_LEN, 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue;  // minimum DNS header size

        // Build DNS response: copy header + question, add answer
        memcpy(tx_buf, rx_buf, len);

        // Set response flags: QR=1, AA=1, RA=1
        tx_buf[2] = 0x84;  // QR=1, Opcode=0, AA=1
        tx_buf[3] = 0x00;  // RA=0, RCODE=0
        // Set answer count to 1
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x01;

        int pos = len;

        // Answer: pointer to question name + A record pointing to 192.168.4.1
        tx_buf[pos++] = 0xC0;  // name pointer
        tx_buf[pos++] = 0x0C;  // offset to question name
        tx_buf[pos++] = 0x00;  // type A
        tx_buf[pos++] = 0x01;
        tx_buf[pos++] = 0x00;  // class IN
        tx_buf[pos++] = 0x01;
        tx_buf[pos++] = 0x00;  // TTL = 60 seconds
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x3C;
        tx_buf[pos++] = 0x00;  // rdlength = 4
        tx_buf[pos++] = 0x04;
        tx_buf[pos++] = 192;   // 192.168.4.1
        tx_buf[pos++] = 168;
        tx_buf[pos++] = 4;
        tx_buf[pos++] = 1;

        sendto(sock, tx_buf, pos, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }
}

static void start_dns_server(void)
{
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
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

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.ssid, WIFI_SSID, strlen(WIFI_SSID));
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    wifi_config.ap.channel = WIFI_CHANNEL;
    wifi_config.ap.max_connection = MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started — SSID: %s, Channel: %d", WIFI_SSID, WIFI_CHANNEL);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
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

    // Create PunchMeter communication primitives
    score_queue = xQueueCreate(SCORE_QUEUE_SIZE, sizeof(score_event_t));
    config_mutex = xSemaphoreCreateMutex();

    // Start PunchMeter on core 1
    xTaskCreatePinnedToCore(punchmeter_task, "punchmeter", 4096, NULL, 5, NULL, 1);

    // Start servers
    start_webserver();
    start_dns_server();

    // Start serial test input task
    xTaskCreate(serial_test_task, "serial_test", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "Running on partition: %s", running->label);
}

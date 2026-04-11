#include "wifi_sta.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_evt = NULL;

static void evt_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Disconnected (reason=%d), retrying", e ? e->reason : -1);
        xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_start_and_wait(void)
{
    s_evt = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &evt_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &evt_handler, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_BAPTHIT_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_BAPTHIT_WIFI_PASS, sizeof(wc.sta.password) - 1);
    // Allow WPA/WPA2/WPA3 — some home routers still run WPA1, and ESP-IDF
    // auto-promotes OPEN to WPA2 whenever a password is provided, which
    // breaks WPA1-only APs (disconnect reason 211).
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for IP on SSID '%s'...", CONFIG_BAPTHIT_WIFI_SSID);
    xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

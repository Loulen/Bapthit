#include "ws_client.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t s_client = NULL;
static ws_on_text_fn    s_on_text = NULL;
static ws_on_connect_fn s_on_connect = NULL;
static volatile bool    s_connected = false;

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "CONNECTED");
        s_connected = true;
        if (s_on_connect) s_on_connect();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "DISCONNECTED");
        s_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 /* text frame */ && d->data_len > 0 && s_on_text) {
            s_on_text((const char *)d->data_ptr, d->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "ERROR");
        s_connected = false;
        break;
    default:
        break;
    }
}

esp_err_t ws_client_start(const ws_client_cfg_t *cfg)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s", cfg->host, cfg->port, cfg->path);

    esp_websocket_client_config_t wc = {0};
    wc.uri = uri;
    wc.reconnect_timeout_ms = 5000;
    wc.network_timeout_ms = 10000;

    s_client = esp_websocket_client_init(&wc);
    if (!s_client) return ESP_FAIL;

    s_on_text = cfg->on_text;
    s_on_connect = cfg->on_connect;

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    return esp_websocket_client_start(s_client);
}

bool ws_client_is_connected(void)
{
    return s_connected;
}

esp_err_t ws_client_send_text(const char *data, size_t len)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    int sent = esp_websocket_client_send_text(s_client, data, (int)len, pdMS_TO_TICKS(3000));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "bapthit";

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
        // TODO: add self-diagnostics here
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA firmware validated");
    }

    ESP_LOGI(TAG, "Running on partition: %s", running->label);
}

#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start WiFi in STA mode using CONFIG_BAPTHIT_WIFI_SSID/PASS.
 * Blocks until an IP is obtained, with infinite retries on disconnect.
 */
esp_err_t wifi_sta_start_and_wait(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ws_on_text_fn)(const char *data, size_t len);
typedef void (*ws_on_connect_fn)(void);

typedef struct {
    const char       *host;       /* e.g. "bapthit-server.local" */
    int               port;       /* e.g. 6969 */
    const char       *path;       /* e.g. "/ws/device" */
    ws_on_connect_fn  on_connect;
    ws_on_text_fn     on_text;
} ws_client_cfg_t;

/** Start the WS client. Auto-reconnect is handled internally. */
esp_err_t ws_client_start(const ws_client_cfg_t *cfg);

/** True if the WS is currently connected (last known state). */
bool ws_client_is_connected(void);

/** Send a UTF-8 text frame (JSON). Returns ESP_OK on success. */
esp_err_t ws_client_send_text(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif

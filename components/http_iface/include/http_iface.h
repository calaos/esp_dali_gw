/**
 * @file http_iface.h
 * @brief esp_http_server adapter: REST, SSE, static UI, captive portal, OTA (SPEC 9).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_IFACE_MAX_SSE_CLIENTS 3 /**< SPEC 9; extra clients are refused, not queued. */

esp_err_t http_iface_init(void);
esp_err_t http_iface_stop(void);

/**
 * @brief Push one SSE frame to every subscribed client.
 *
 * Non-blocking: a client whose socket would block is dropped rather than stalling the httpd task.
 *
 * @param event  SSE event name: "gear", "bus", "progress", "result", "log"
 * @param json   serialized payload
 */
void http_iface_sse_broadcast(const char *event, const char *json);

#ifdef __cplusplus
}
#endif

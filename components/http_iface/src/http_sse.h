/** Server-sent events stream (SPEC 9). Private to http_iface. */
#pragma once

#include "esp_http_server.h"

/** @brief Remember the server handle and start the heartbeat. */
esp_err_t http_sse_start(httpd_handle_t server);

void http_sse_stop(void);

/** @brief The GET /api/events handler. */
esp_err_t http_sse_open(httpd_req_t *req);

/** @brief Push one frame to every subscriber. Never blocks; a stalled client is dropped. */
void http_sse_broadcast(const char *event, const char *json);

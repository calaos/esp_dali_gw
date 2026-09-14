/** Optional HTTP Basic auth (SPEC 9). Private to http_iface. */
#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

/**
 * @brief Reject the request unless it carries valid credentials.
 *
 * A no-op when http.auth is disabled. Sends the 401 challenge itself.
 *
 * @retval true  the handler may proceed
 */
bool http_auth_ok(httpd_req_t *req);

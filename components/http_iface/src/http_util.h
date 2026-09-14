/** Request helpers shared by the route handlers. Private to http_iface. */
#pragma once

#include <stddef.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "gw_api.h"

/** Largest request body accepted on a JSON route. A config document is ~2 KB. */
#define HTTP_MAX_BODY 8192

/**
 * @brief Read the whole request body into a NUL-terminated heap buffer.
 *
 * @param[out] out  freed by the caller with free(); NULL on failure
 * @retval ESP_ERR_INVALID_SIZE  body larger than HTTP_MAX_BODY
 */
esp_err_t http_read_body(httpd_req_t *req, char **out, size_t *len);

/** @brief Read the body and parse it as JSON. Replies 400 itself on malformed input. */
esp_err_t http_read_json(httpd_req_t *req, cJSON **out);

/** @brief Send @p obj as the response and delete it. Replies 500 if @p obj is NULL. */
esp_err_t http_send_json(httpd_req_t *req, cJSON *obj);

/** @brief Send `{"ok":false,"error":...,"message":...}` with a status matching @p err. */
esp_err_t http_send_error(httpd_req_t *req, gw_err_t err, const char *message);

/**
 * @brief Same, with an explicit status line.
 *
 * For the few failures that are transport-level rather than command-level -- an authentication
 * challenge, a refused subscriber -- where the closed error set does not imply the right status.
 */
esp_err_t http_send_error_status(httpd_req_t *req, const char *status, gw_err_t err,
                                 const char *message);

/** @brief True when the query string contains @p key set to 1 or true. */
bool http_query_flag(httpd_req_t *req, const char *key);

/** @brief True when the body is `{"confirm": true}`. */
bool http_confirmed(const cJSON *root);

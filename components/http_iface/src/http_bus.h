/** Bus, gear and group routes. Private to http_iface. */
#pragma once

#include "esp_http_server.h"

esp_err_t http_route_bus_get(httpd_req_t *req);
esp_err_t http_route_bus_scan(httpd_req_t *req);
esp_err_t http_route_bus_commission(httpd_req_t *req);
esp_err_t http_route_bus_cancel(httpd_req_t *req);
esp_err_t http_route_bus_check(httpd_req_t *req);
esp_err_t http_route_bus_raw(httpd_req_t *req);
esp_err_t http_route_bus_query(httpd_req_t *req);

esp_err_t http_route_gears_get(httpd_req_t *req);
esp_err_t http_route_gear_get(httpd_req_t *req);
esp_err_t http_route_gear_set(httpd_req_t *req);
esp_err_t http_route_group_set(httpd_req_t *req);
esp_err_t http_route_broadcast_set(httpd_req_t *req);
esp_err_t http_route_gear_post(httpd_req_t *req);
esp_err_t http_route_group_post(httpd_req_t *req);
esp_err_t http_route_rename(httpd_req_t *req);

/** @brief Bridge DALI_GW_EVENT onto the SSE stream. */
esp_err_t http_bus_events_subscribe(void);

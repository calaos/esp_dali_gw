/** Route handlers, split out of http_iface.c to keep the server setup readable. */
#pragma once

#include "esp_http_server.h"

esp_err_t http_route_config_get(httpd_req_t *req);
esp_err_t http_route_config_put(httpd_req_t *req);
esp_err_t http_route_config_export(httpd_req_t *req);
esp_err_t http_route_config_import(httpd_req_t *req);
esp_err_t http_route_wifi_scan(httpd_req_t *req);
esp_err_t http_route_reboot(httpd_req_t *req);
esp_err_t http_route_factory_reset(httpd_req_t *req);
esp_err_t http_route_ota(httpd_req_t *req);

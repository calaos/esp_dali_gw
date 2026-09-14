#include <string.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "app_config.h"
#include "gw_api.h"
#include "http_iface.h"
#include "net_wifi.h"
#include "webui.h"

static const char *TAG = "http";

static httpd_handle_t s_server;

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    if (obj == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "serialization failed");
        return ESP_FAIL;
    }
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static void collect_info(gw_info_t *info)
{
    memset(info, 0, sizeof(*info));

    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(info->state, "online", sizeof(info->state));
    strlcpy(info->fw, app->version, sizeof(info->fw));
    strlcpy(info->idf, IDF_VER, sizeof(info->idf));
    snprintf(info->build_date, sizeof(info->build_date), "%s %s", app->date, app->time);

    net_wifi_status_t net;
    net_wifi_get_status(&net);
    strlcpy(info->ip, net.ip, sizeof(info->ip));
    strlcpy(info->hostname, net.hostname, sizeof(info->hostname));
    strlcpy(info->mode, net.mode, sizeof(info->mode));
    info->rssi = net.rssi;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(info->mac, sizeof(info->mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    app_config_device_id(info->device_id, sizeof(info->device_id));

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(info->chip, sizeof(info->chip), "esp32c6 rev%d.%d", chip.revision / 100,
             chip.revision % 100);

    info->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    info->free_heap = esp_get_free_heap_size();
    info->min_free_heap = esp_get_minimum_free_heap_size();
    info->reset_reason = (int)esp_reset_reason();
}

static esp_err_t info_get(httpd_req_t *req)
{
    gw_info_t info;
    collect_info(&info);
    return send_json(req, gw_api_info_to_json(&info));
}

/**
 * Every asset is stored gzipped, so the response is the embedded blob verbatim with
 * Content-Encoding: gzip. The ETag is the bundle build hash: it changes only when an asset changes,
 * which lets a browser keep the whole UI cached across reboots.
 */
static esp_err_t static_get(httpd_req_t *req)
{
    const webui_asset_t *asset = webui_find(req->uri);
    if (asset == NULL) {
        // TODO(M1): in AP mode, redirect unknown paths to / for the captive portal (SPEC 5.2).
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    char etag[24];
    snprintf(etag, sizeof(etag), "\"%s\"", webui_build_hash());

    char inm[32];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, asset->content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)asset->data, asset->size);
}

esp_err_t http_iface_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* SSE clients hold their socket open; without LRU purge they would starve normal requests. */
    cfg.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    static const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_get, .user_ctx = NULL},
        {.uri = "/*", .method = HTTP_GET, .handler = static_get, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "register %s",
                            routes[i].uri);
    }

    ESP_LOGI(TAG, "http server up on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t http_iface_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

void http_iface_sse_broadcast(const char *event, const char *json)
{
    // TODO(M2): fan out to the subscribed /api/events clients, dropping any that would block.
    (void)event;
    (void)json;
}

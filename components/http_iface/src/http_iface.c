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
#include "http_bus.h"
#include "http_routes.h"
#include "http_sse.h"
#include "http_util.h"
#include "webui.h"

static const char *TAG = "http";

static httpd_handle_t s_server;

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
    return http_send_json(req, gw_api_info_to_json(&info));
}

/**
 * Every asset is stored gzipped, so the response is the embedded blob verbatim with
 * Content-Encoding: gzip. The ETag is the bundle build hash: it changes only when an asset changes,
 * which lets a browser keep the whole UI cached across reboots.
 */
/* Registration of the handler past this limit fails at boot, not at build time. */
#define HTTP_MAX_HANDLERS 32

#define AP_PORTAL_URL "http://192.168.4.1/"

/** OS connectivity probes. Answering them with a redirect is what pops the captive-portal sheet. */
static bool is_captive_probe(const char *path)
{
    static const char *probes[] = {
        "/generate_204", "/gen_204",        "/hotspot-detect.html", "/connecttest.txt",
        "/ncsi.txt",     "/canonical.html", "/success.txt",         "/redirect",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (strcmp(path, probes[i]) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * In APSTA the device answers on the AP address *and* on its LAN address, so a foreign Host is the
 * only reliable signal that a client arrived through the portal rather than by typing our name.
 */
static bool host_is_ours(httpd_req_t *req)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    char *colon = strchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
    }
    if (strcmp(host, "192.168.4.1") == 0) {
        return true;
    }

    net_wifi_status_t net;
    net_wifi_get_status(&net);
    if (net.ip[0] != '\0' && strcmp(host, net.ip) == 0) {
        return true;
    }
    size_t hn = strlen(net.hostname);
    if (hn > 0 && strncasecmp(host, net.hostname, hn) == 0 &&
        (host[hn] == '\0' || strcasecmp(host + hn, ".local") == 0)) {
        return true;
    }
    return false;
}

static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", AP_PORTAL_URL);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t static_get(httpd_req_t *req)
{
    if (net_wifi_ap_active() && (is_captive_probe(req->uri) || !host_is_ours(req))) {
        return redirect_to_portal(req);
    }

    const webui_asset_t *asset = webui_find(req->uri);
    if (asset == NULL) {
        /* A path with no extension is a client-side route, not a missing file: hand it the SPA. */
        if (strchr(req->uri, '.') == NULL) {
            asset = webui_index();
        }
        if (asset == NULL) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
            return ESP_FAIL;
        }
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
    cfg.max_uri_handlers = HTTP_MAX_HANDLERS;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* SSE clients hold their socket open; without LRU purge they would starve normal requests. */
    cfg.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    static const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_get, .user_ctx = NULL},
        {.uri = "/api/config",
         .method = HTTP_GET,
         .handler = http_route_config_get,
         .user_ctx = NULL},
        {.uri = "/api/config",
         .method = HTTP_PUT,
         .handler = http_route_config_put,
         .user_ctx = NULL},
        {.uri = "/api/config/export",
         .method = HTTP_POST,
         .handler = http_route_config_export,
         .user_ctx = NULL},
        {.uri = "/api/config/import",
         .method = HTTP_POST,
         .handler = http_route_config_import,
         .user_ctx = NULL},
        {.uri = "/api/wifi/scan",
         .method = HTTP_POST,
         .handler = http_route_wifi_scan,
         .user_ctx = NULL},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = http_route_ota, .user_ctx = NULL},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = http_route_reboot, .user_ctx = NULL},
        {.uri = "/api/factory_reset",
         .method = HTTP_POST,
         .handler = http_route_factory_reset,
         .user_ctx = NULL},
        {.uri = "/api/events", .method = HTTP_GET, .handler = http_sse_open, .user_ctx = NULL},

        {.uri = "/api/bus", .method = HTTP_GET, .handler = http_route_bus_get, .user_ctx = NULL},
        {.uri = "/api/bus/scan",
         .method = HTTP_POST,
         .handler = http_route_bus_scan,
         .user_ctx = NULL},
        {.uri = "/api/bus/commission",
         .method = HTTP_POST,
         .handler = http_route_bus_commission,
         .user_ctx = NULL},
        {.uri = "/api/bus/cancel",
         .method = HTTP_POST,
         .handler = http_route_bus_cancel,
         .user_ctx = NULL},
        {.uri = "/api/bus/check",
         .method = HTTP_POST,
         .handler = http_route_bus_check,
         .user_ctx = NULL},
        {.uri = "/api/bus/raw",
         .method = HTTP_POST,
         .handler = http_route_bus_raw,
         .user_ctx = NULL},
        {.uri = "/api/bus/query",
         .method = HTTP_POST,
         .handler = http_route_bus_query,
         .user_ctx = NULL},

        {.uri = "/api/gears",
         .method = HTTP_GET,
         .handler = http_route_gears_get,
         .user_ctx = NULL},
        {.uri = "/api/gears/*",
         .method = HTTP_POST,
         .handler = http_route_gear_post,
         .user_ctx = NULL},
        {.uri = "/api/groups/*",
         .method = HTTP_POST,
         .handler = http_route_group_post,
         .user_ctx = NULL},
        {.uri = "/api/broadcast/set",
         .method = HTTP_POST,
         .handler = http_route_broadcast_set,
         .user_ctx = NULL},
        {.uri = "/api/gears/*",
         .method = HTTP_PATCH,
         .handler = http_route_rename,
         .user_ctx = NULL},
        {.uri = "/api/groups/*",
         .method = HTTP_PATCH,
         .handler = http_route_rename,
         .user_ctx = NULL},
        /* After the more specific per-gear routes so this wildcard cannot shadow them. */
        {.uri = "/api/gears/*",
         .method = HTTP_GET,
         .handler = http_route_gear_get,
         .user_ctx = NULL},

        /* Last: the wildcard would otherwise swallow every GET above it. */
        {.uri = "/*", .method = HTTP_GET, .handler = static_get, .user_ctx = NULL},
    };
    _Static_assert(sizeof(routes) / sizeof(routes[0]) <= HTTP_MAX_HANDLERS,
                   "more routes than httpd is configured to hold");

    /*
     * httpd_uri_match_wildcard() only honours a '*' in the final position: anywhere else it is
     * compared literally, so a template with an inner wildcard silently matches nothing and every
     * request to it 404s. Catch that here rather than on a bench.
     */
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const char *star = strchr(routes[i].uri, '*');
        ESP_RETURN_ON_FALSE(star == NULL || star[1] == '\0', ESP_ERR_INVALID_ARG, TAG,
                            "route %s has a wildcard that is not in the last position",
                            routes[i].uri);
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "register %s",
                            routes[i].uri);
    }

    ESP_RETURN_ON_ERROR(http_sse_start(s_server), TAG, "sse start");
    ESP_RETURN_ON_ERROR(http_bus_events_subscribe(), TAG, "event subscribe");

    ESP_LOGI(TAG, "http server up on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t http_iface_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    http_sse_stop();
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

void http_iface_sse_broadcast(const char *event, const char *json)
{
    http_sse_broadcast(event, json);
}

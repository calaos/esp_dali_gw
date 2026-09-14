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
#include "http_auth.h"
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
    snprintf(info->chip, sizeof(info->chip), "%s rev%d.%d", CONFIG_IDF_TARGET, chip.revision / 100,
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

/** A client that arrived through the portal has not seen the UI yet and cannot have logged in. */
static bool captive_redirect_due(httpd_req_t *req)
{
    return net_wifi_ap_active() && (is_captive_probe(req->uri) || !host_is_ours(req));
}

static esp_err_t static_get(httpd_req_t *req)
{
    if (captive_redirect_due(req)) {
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

/** What a registered route really points at; httpd's user_ctx carries it. */
typedef struct {
    esp_err_t (*fn)(httpd_req_t *req);
} route_handler_t;

/**
 * Auth is enforced in one place: a per-handler check is one new route away from being forgotten.
 *
 * The only exemption is the captive-portal redirect (SPEC 9). An OS connectivity probe cannot
 * carry credentials, and answering it with a 401 makes the phone decide the network is broken
 * instead of opening the portal sheet.
 */
static esp_err_t authed(httpd_req_t *req)
{
    if (captive_redirect_due(req)) {
        return redirect_to_portal(req);
    }
    if (!http_auth_ok(req)) {
        return ESP_OK; /* the challenge has already been sent */
    }
    return ((const route_handler_t *)req->user_ctx)->fn(req);
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

    static const route_handler_t ctx_info_get = {info_get};
    static const route_handler_t ctx_http_route_config_get = {http_route_config_get};
    static const route_handler_t ctx_http_route_config_put = {http_route_config_put};
    static const route_handler_t ctx_http_route_config_export = {http_route_config_export};
    static const route_handler_t ctx_http_route_config_import = {http_route_config_import};
    static const route_handler_t ctx_http_route_wifi_scan = {http_route_wifi_scan};
    static const route_handler_t ctx_http_route_ota = {http_route_ota};
    static const route_handler_t ctx_http_route_reboot = {http_route_reboot};
    static const route_handler_t ctx_http_route_factory_reset = {http_route_factory_reset};
    static const route_handler_t ctx_http_sse_open = {http_sse_open};
    static const route_handler_t ctx_http_route_bus_get = {http_route_bus_get};
    static const route_handler_t ctx_http_route_bus_scan = {http_route_bus_scan};
    static const route_handler_t ctx_http_route_bus_commission = {http_route_bus_commission};
    static const route_handler_t ctx_http_route_bus_cancel = {http_route_bus_cancel};
    static const route_handler_t ctx_http_route_bus_check = {http_route_bus_check};
    static const route_handler_t ctx_http_route_bus_raw = {http_route_bus_raw};
    static const route_handler_t ctx_http_route_bus_query = {http_route_bus_query};
    static const route_handler_t ctx_http_route_bus_monitor = {http_route_bus_monitor};
    static const route_handler_t ctx_http_route_gears_get = {http_route_gears_get};
    static const route_handler_t ctx_http_route_gear_post = {http_route_gear_post};
    static const route_handler_t ctx_http_route_group_post = {http_route_group_post};
    static const route_handler_t ctx_http_route_broadcast_set = {http_route_broadcast_set};
    static const route_handler_t ctx_http_route_rename = {http_route_rename};
    static const route_handler_t ctx_http_route_gear_get = {http_route_gear_get};
    static const route_handler_t ctx_static_get = {static_get};

    static const httpd_uri_t routes[] = {
        {.uri = "/api/info",
         .method = HTTP_GET,
         .handler = authed,
         .user_ctx = (void *)&ctx_info_get},
        {.uri = "/api/config",
         .method = HTTP_GET,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_config_get},
        {.uri = "/api/config",
         .method = HTTP_PUT,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_config_put},
        {.uri = "/api/config/export",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_config_export},
        {.uri = "/api/config/import",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_config_import},
        {.uri = "/api/wifi/scan",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_wifi_scan},
        {.uri = "/api/ota",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_ota},
        {.uri = "/api/reboot",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_reboot},
        {.uri = "/api/factory_reset",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_factory_reset},
        {.uri = "/api/events",
         .method = HTTP_GET,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_sse_open},
        {.uri = "/api/bus",
         .method = HTTP_GET,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_get},
        {.uri = "/api/bus/scan",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_scan},
        {.uri = "/api/bus/commission",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_commission},
        {.uri = "/api/bus/cancel",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_cancel},
        {.uri = "/api/bus/check",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_check},
        {.uri = "/api/bus/raw",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_raw},
        {.uri = "/api/bus/query",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_query},
        {.uri = "/api/bus/monitor",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_bus_monitor},
        {.uri = "/api/gears",
         .method = HTTP_GET,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_gears_get},
        {.uri = "/api/gears/*",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_gear_post},
        {.uri = "/api/groups/*",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_group_post},
        {.uri = "/api/broadcast/set",
         .method = HTTP_POST,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_broadcast_set},
        {.uri = "/api/gears/*",
         .method = HTTP_PATCH,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_rename},
        {.uri = "/api/groups/*",
         .method = HTTP_PATCH,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_rename},
        {.uri = "/api/gears/*",
         .method = HTTP_GET,
         .handler = authed,
         .user_ctx = (void *)&ctx_http_route_gear_get},
        {.uri = "/*", .method = HTTP_GET, .handler = authed, .user_ctx = (void *)&ctx_static_get},
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

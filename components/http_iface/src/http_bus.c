/** Bus, gear and group routes (SPEC 9), plus the bridge from DALI_GW_EVENT onto the SSE stream. */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "dali_bus.h"
#include "gw_api.h"
#include "gw_events.h"
#include "http_bus.h"
#include "http_sse.h"
#include "http_util.h"

/** SPEC 9: the synchronous endpoints answer in-band or give up. */
#define SYNC_TIMEOUT_MS 2000

/* --- helpers --------------------------------------------------------------------------------- */

/**
 * Segment following the number in "/api/gears/3/configure", or "" when there is none.
 *
 * esp_http_server's wildcard matcher only honours a trailing star, so a template with an inner
 * wildcard never matches anything. The routes are registered with a trailing wildcard and the
 * sub-path is dispatched here instead.
 */
static const char *uri_action_after(const char *uri, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) {
        return "";
    }
    const char *p = uri + plen;
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (*p != '/') {
        return "";
    }
    return p + 1;
}

/** Extract a decimal number from the URI segment after @p prefix. Returns -1 when absent. */
static int uri_number_after(const char *uri, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) {
        return -1;
    }
    const char *p = uri + plen;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 999) {
            return -1;
        }
        p++;
    }
    return value;
}

/** Run a command through the bus and answer with its result. Takes no ownership of @p cmd. */
static esp_err_t run_sync(httpd_req_t *req, gw_cmd_t *cmd)
{
    cmd->origin = GW_ORIGIN_HTTP;

    gw_result_t res;
    memset(&res, 0, sizeof(res));

    esp_err_t err = dali_bus_submit_sync(cmd, &res, SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_NO_MEM) {
        return http_send_error(req, GW_ERR_BUS_BUSY, "command queue full");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return http_send_error(req, GW_ERR_TIMEOUT, "the bus did not answer in time");
    }
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not reach the bus");
    }

    cJSON *obj = gw_api_result_to_json(&res);
    gw_api_result_free(&res);
    if (!res.ok) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return http_send_json(req, obj);
}

/** Start a long operation: 202 with {started:true}, progress arrives on SSE (SPEC 9). */
static esp_err_t run_async(httpd_req_t *req, gw_cmd_t *cmd)
{
    cmd->origin = GW_ORIGIN_HTTP;

    gw_result_t res;
    memset(&res, 0, sizeof(res));
    esp_err_t err = dali_bus_submit_sync(cmd, &res, SYNC_TIMEOUT_MS);
    if (err == ESP_ERR_NO_MEM) {
        return http_send_error(req, GW_ERR_BUS_BUSY, "command queue full");
    }
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not start the operation");
    }

    cJSON *obj = gw_api_result_to_json(&res);
    bool ok = res.ok;
    gw_api_result_free(&res);
    httpd_resp_set_status(req, ok ? "202 Accepted" : "409 Conflict");
    return http_send_json(req, obj);
}

/** Read the body, parse it as a cmd/<action> payload, and hand it to @p runner. */
static esp_err_t route_action(httpd_req_t *req, const char *action,
                              esp_err_t (*runner)(httpd_req_t *, gw_cmd_t *))
{
    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }

    gw_cmd_t cmd;
    char message[96] = {0};
    esp_err_t err = gw_api_cmd_from_json(action, root, &cmd, message, sizeof(message));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return http_send_error(req, GW_ERR_INVALID_ARG, message[0] ? message : "bad payload");
    }
    return runner(req, &cmd);
}

/* --- bus ------------------------------------------------------------------------------------- */

esp_err_t http_route_bus_get(httpd_req_t *req)
{
    gw_bus_status_t bus;
    dali_bus_get_status(&bus);
    return http_send_json(req, gw_api_bus_to_json(&bus));
}

esp_err_t http_route_bus_scan(httpd_req_t *req)
{
    return route_action(req, "scan", run_async);
}

esp_err_t http_route_bus_commission(httpd_req_t *req)
{
    return route_action(req, "commission", run_async);
}

esp_err_t http_route_bus_cancel(httpd_req_t *req)
{
    esp_err_t err = dali_bus_cancel();
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not queue the cancel");
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    return http_send_json(req, res);
}

esp_err_t http_route_bus_check(httpd_req_t *req)
{
    return route_action(req, "bus_check", run_sync);
}

esp_err_t http_route_bus_raw(httpd_req_t *req)
{
    return route_action(req, "raw", run_sync);
}

esp_err_t http_route_bus_query(httpd_req_t *req)
{
    return route_action(req, "query", run_sync);
}

/* --- gears ----------------------------------------------------------------------------------- */

esp_err_t http_route_gears_get(httpd_req_t *req)
{
    static gw_gear_t gears[GW_MAX_GEARS];
    size_t count = 0;
    esp_err_t err = dali_bus_get_gears(gears, GW_MAX_GEARS, &count);
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "registry unavailable");
    }
    return http_send_json(req, gw_api_gears_to_json(gears, count));
}

esp_err_t http_route_gear_get(httpd_req_t *req)
{
    int addr = uri_number_after(req->uri, "/api/gears/");
    if (addr < 0 || addr >= GW_MAX_GEARS) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "short address must be 0..63");
    }
    gw_gear_t gear;
    if (dali_bus_get_gear((uint8_t)addr, &gear) != ESP_OK) {
        return http_send_error(req, GW_ERR_NOT_PRESENT, "unknown short address");
    }
    return http_send_json(req, gw_api_gear_to_json(&gear, true));
}

/** The `set` payload forms are shared with MQTT, so they go through the same parser. */
static esp_err_t route_set(httpd_req_t *req, gw_target_t target)
{
    char *body = NULL;
    size_t len = 0;
    if (http_read_body(req, &body, &len) != ESP_OK) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "cannot read body");
    }

    gw_cmd_t cmd;
    char message[96] = {0};
    esp_err_t err = gw_api_set_from_payload(body, len, target, &cmd, message, sizeof(message));
    free(body);
    if (err != ESP_OK) {
        return http_send_error(req, GW_ERR_INVALID_ARG, message[0] ? message : "bad payload");
    }
    return run_sync(req, &cmd);
}

esp_err_t http_route_gear_set(httpd_req_t *req)
{
    int addr = uri_number_after(req->uri, "/api/gears/");
    if (addr < 0 || addr >= GW_MAX_GEARS) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "short address must be 0..63");
    }
    const gw_target_t target = {.type = GW_TARGET_SHORT, .addr = (uint8_t)addr};
    return route_set(req, target);
}

esp_err_t http_route_group_set(httpd_req_t *req)
{
    int group = uri_number_after(req->uri, "/api/groups/");
    if (group < 0 || group >= GW_MAX_GROUPS) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "group must be 0..15");
    }
    const gw_target_t target = {.type = GW_TARGET_GROUP, .addr = (uint8_t)group};
    return route_set(req, target);
}

esp_err_t http_route_broadcast_set(httpd_req_t *req)
{
    const gw_target_t target = {.type = GW_TARGET_BROADCAST, .addr = 0};
    return route_set(req, target);
}

/** The per-gear action routes all carry the address in the path and the rest in the body. */
static esp_err_t route_gear_action(httpd_req_t *req, const char *action,
                                   esp_err_t (*runner)(httpd_req_t *, gw_cmd_t *))
{
    int addr = uri_number_after(req->uri, "/api/gears/");
    if (addr < 0 || addr >= GW_MAX_GEARS) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "short address must be 0..63");
    }

    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    /* The shared parser takes the address from the body, so put the path's address there. */
    cJSON_DeleteItemFromObject(root, "addr");
    cJSON_AddNumberToObject(root, "addr", addr);

    gw_cmd_t cmd;
    char message[96] = {0};
    esp_err_t err = gw_api_cmd_from_json(action, root, &cmd, message, sizeof(message));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return http_send_error(req, GW_ERR_INVALID_ARG, message[0] ? message : "bad payload");
    }
    return runner(req, &cmd);
}

/** POST /api/bus/monitor -- turn passive listening on or off (M5). */
esp_err_t http_route_bus_monitor(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(root);
        return http_send_error(req, GW_ERR_INVALID_ARG, "enabled must be a boolean");
    }
    esp_err_t err = dali_bus_listen_set(cJSON_IsTrue(enabled));
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not change listen mode");
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    cJSON_AddBoolToObject(res, "listening", dali_bus_listen_active());
    return http_send_json(req, res);
}

/** POST /api/gears/<addr>/<action> -- the address is in the path, the arguments in the body. */
esp_err_t http_route_gear_post(httpd_req_t *req)
{
    const char *action = uri_action_after(req->uri, "/api/gears/");

    if (strcmp(action, "set") == 0) {
        return http_route_gear_set(req);
    }
    if (strcmp(action, "configure") == 0) {
        /* configure is a state machine but parks its caller, so it still answers in band. */
        return route_gear_action(req, "configure", run_sync);
    }
    if (strcmp(action, "identify") == 0) {
        return route_gear_action(req, "identify", run_async);
    }
    if (strcmp(action, "address") == 0) {
        return route_gear_action(req, "set_short_address", run_sync);
    }
    if (strcmp(action, "remove_address") == 0) {
        return route_gear_action(req, "remove_short_address", run_sync);
    }
    return http_send_error(req, GW_ERR_NOT_PRESENT, "unknown gear action");
}

/** POST /api/groups/<n>/set. */
esp_err_t http_route_group_post(httpd_req_t *req)
{
    if (strcmp(uri_action_after(req->uri, "/api/groups/"), "set") != 0) {
        return http_send_error(req, GW_ERR_NOT_PRESENT, "unknown group action");
    }
    return http_route_group_set(req);
}

/** Renaming is registry and configuration only: it never touches the bus. */
esp_err_t http_route_rename(httpd_req_t *req)
{
    bool is_group = strncmp(req->uri, "/api/groups/", 12) == 0;
    int index = uri_number_after(req->uri, is_group ? "/api/groups/" : "/api/gears/");
    int limit = is_group ? GW_MAX_GROUPS : GW_MAX_GEARS;
    if (index < 0 || index >= limit) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "index out of range");
    }

    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name)) {
        cJSON_Delete(root);
        return http_send_error(req, GW_ERR_INVALID_ARG, "name must be a string");
    }

    esp_err_t err = is_group ? app_config_set_group_name((uint8_t)index, name->valuestring)
                             : app_config_set_gear_name((uint8_t)index, name->valuestring);
    if (err == ESP_OK && !is_group) {
        dali_bus_set_gear_name((uint8_t)index, name->valuestring);
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not persist the name");
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    return http_send_json(req, res);
}

/* --- events ---------------------------------------------------------------------------------- */

/** Serialize an event once and push it to every subscriber. */
static void broadcast(const char *name, cJSON *obj)
{
    if (obj == NULL) {
        return;
    }
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        return;
    }
    http_sse_broadcast(name, text);
    cJSON_free(text);
}

static void on_gw_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch ((gw_event_id_t)id) {
        case GW_EVENT_GEAR_CHANGED: {
            gw_gear_t gear;
            if (dali_bus_get_gear(((const gw_event_gear_t *)data)->addr, &gear) == ESP_OK) {
                broadcast("gear", gw_api_gear_to_json(&gear, false));
            }
            break;
        }
        case GW_EVENT_BUS_STATE: {
            gw_bus_status_t bus;
            dali_bus_get_status(&bus);
            broadcast("bus", gw_api_bus_to_json(&bus));
            break;
        }
        case GW_EVENT_PROGRESS: {
            const gw_event_progress_t *ev = data;
            broadcast("progress",
                      gw_api_progress_to_json(ev->operation, ev->done, ev->total, ev->found));
            break;
        }
        case GW_EVENT_RESULT: {
            const gw_event_result_t *ev = data;
            gw_result_t res = {
                .id = ev->id,
                .ok = ev->ok,
                .error = ev->error,
                .has_target = ev->has_target,
                .target = ev->target,
                .duration_ms = ev->duration_ms,
            };
            strlcpy(res.action, ev->action, sizeof(res.action));
            broadcast("result", gw_api_result_to_json(&res));
            break;
        }
        case GW_EVENT_RX: {
            const gw_event_rx_t *ev = data;
            cJSON *obj = cJSON_CreateObject();
            if (obj != NULL) {
                char hex[9];
                snprintf(hex, sizeof(hex), "%0*" PRIX32, ev->bits / 4, ev->frame);
                cJSON_AddStringToObject(obj, "frame", hex);
                cJSON_AddNumberToObject(obj, "bits", ev->bits);
                cJSON_AddNumberToObject(obj, "ts", (double)(ev->timestamp_us / 1000));
                broadcast("rx", obj);
            }
            break;
        }
        case GW_EVENT_LOG: {
            const gw_event_log_t *ev = data;
            cJSON *obj = cJSON_CreateObject();
            if (obj != NULL) {
                cJSON_AddStringToObject(obj, "level", ev->level);
                cJSON_AddStringToObject(obj, "msg", ev->msg);
                broadcast("log", obj);
            }
            break;
        }
        default:
            break;
    }
}

esp_err_t http_bus_events_subscribe(void)
{
    return esp_event_handler_register(DALI_GW_EVENT, ESP_EVENT_ANY_ID, on_gw_event, NULL);
}

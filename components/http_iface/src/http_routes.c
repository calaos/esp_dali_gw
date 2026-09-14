/** REST routes other than /api/info and the static assets (SPEC 9). */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "app_config.h"
#include "gw_api.h"
#include "gw_events.h"
#include "http_routes.h"
#include "http_util.h"
#include "net_wifi.h"

static const char *TAG = "http";

/** Rebooting inside a handler would drop the response the client is still reading. */
static void reboot_later(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void schedule_reboot(void)
{
    xTaskCreate(reboot_later, "reboot", 2048, NULL, 5, NULL);
}

esp_err_t http_route_config_get(httpd_req_t *req)
{
    return http_send_json(req, gw_api_config_to_json(app_config_get(), false));
}

/**
 * A write is a merge onto the live configuration: the UI sends only what changed, and secrets come
 * back as "***" meaning "keep the stored one".
 */
static esp_err_t apply_config(httpd_req_t *req, const cJSON *root)
{
    app_config_t cfg = *app_config_get();
    char field[64] = {0};

    esp_err_t err = gw_api_config_from_json(root, &cfg, field, sizeof(field));
    if (err != ESP_OK) {
        return http_send_error(req, GW_ERR_INVALID_ARG, field[0] ? field : "malformed document");
    }

    app_config_merge_secrets(&cfg);

    err = app_config_validate(&cfg, field, sizeof(field));
    if (err != ESP_OK) {
        return http_send_error(req, GW_ERR_INVALID_ARG, field[0] ? field : "invalid value");
    }

    app_config_impact_t impact = APP_CONFIG_IMPACT_NONE;
    err = app_config_set(&cfg, &impact);
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not persist configuration");
    }

    cJSON *res = cJSON_CreateObject();
    if (res == NULL) {
        return http_send_error(req, GW_ERR_INTERNAL, "out of memory");
    }
    cJSON_AddBoolToObject(res, "ok", true);
    cJSON_AddBoolToObject(res, "reboot_required", (impact & APP_CONFIG_IMPACT_REBOOT) != 0);
    cJSON_AddBoolToObject(res, "mqtt_restart", (impact & APP_CONFIG_IMPACT_MQTT_RESTART) != 0);
    return http_send_json(req, res);
}

esp_err_t http_route_config_put(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = apply_config(req, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t http_route_config_export(httpd_req_t *req)
{
    /* Secrets leave the device only here, only when asked for explicitly (SPEC 6). */
    bool secrets = http_query_flag(req, "secrets");
    if (secrets) {
        ESP_LOGW(TAG, "configuration exported with secrets in clear");
    }
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"esp_dali_gw-config.json\"");
    return http_send_json(req, gw_api_config_to_json(app_config_get(), secrets));
}

esp_err_t http_route_config_import(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = apply_config(req, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t http_route_wifi_scan(httpd_req_t *req)
{
    static net_wifi_ap_t aps[NET_WIFI_MAX_SCAN_RESULTS];
    size_t found = 0;

    esp_err_t err = net_wifi_scan(aps, NET_WIFI_MAX_SCAN_RESULTS, &found, 5000);
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "scan failed");
    }

    cJSON *res = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    if (res == NULL || list == NULL) {
        cJSON_Delete(res);
        cJSON_Delete(list);
        return http_send_error(req, GW_ERR_INTERNAL, "out of memory");
    }
    cJSON_AddItemToObject(res, "networks", list);

    for (size_t i = 0; i < found; i++) {
        cJSON *ap = cJSON_CreateObject();
        if (ap == NULL) {
            break;
        }
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", aps[i].channel);
        cJSON_AddStringToObject(ap, "auth", aps[i].auth);
        cJSON_AddItemToArray(list, ap);
    }
    return http_send_json(req, res);
}

esp_err_t http_route_reboot(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    bool ok = http_confirmed(root);
    cJSON_Delete(root);
    if (!ok) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "confirm must be true");
    }

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    esp_err_t err = http_send_json(req, res);
    ESP_LOGW(TAG, "reboot requested over HTTP");
    schedule_reboot();
    return err;
}

esp_err_t http_route_factory_reset(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_read_json(req, &root) != ESP_OK) {
        return ESP_FAIL;
    }
    bool ok = http_confirmed(root);
    cJSON_Delete(root);
    if (!ok) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "confirm must be true");
    }

    esp_err_t err = app_config_factory_reset();
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not erase configuration");
    }

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    err = http_send_json(req, res);
    ESP_LOGW(TAG, "factory reset requested over HTTP");
    schedule_reboot();
    return err;
}

/**
 * The image is streamed straight into the inactive slot: 1.9 MB does not fit in RAM. A failure at
 * any point aborts the handle, so a half-written slot is never left bootable.
 */
esp_err_t http_route_ota(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return http_send_error(req, GW_ERR_INTERNAL, "no OTA partition available");
    }
    if (req->content_len == 0 || req->content_len > target->size) {
        return http_send_error(req, GW_ERR_INVALID_ARG, "image size does not fit the OTA slot");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "esp_ota_begin failed");
    }

    gw_event_ota_t ev = {.in_progress = true, .percent = 0, .ok = false};
    gw_event_post(GW_EVENT_OTA, &ev, sizeof(ev));

    char *chunk = malloc(4096);
    if (chunk == NULL) {
        esp_ota_abort(handle);
        return http_send_error(req, GW_ERR_INTERNAL, "out of memory");
    }

    size_t received = 0;
    uint8_t last_pct = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, chunk, 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, chunk, (size_t)r);
        if (err != ESP_OK) {
            break;
        }
        received += (size_t)r;

        uint8_t pct = (uint8_t)(received * 100 / req->content_len);
        if (pct != last_pct) {
            last_pct = pct;
            gw_event_ota_t p = {.in_progress = true, .percent = pct, .ok = false};
            gw_event_post(GW_EVENT_OTA, &p, sizeof(p));
        }
    }
    free(chunk);

    if (err != ESP_OK || received != req->content_len) {
        esp_ota_abort(handle);
        gw_event_ota_t done = {.in_progress = false, .percent = last_pct, .ok = false};
        gw_event_post(GW_EVENT_OTA, &done, sizeof(done));
        return http_send_error(req, GW_ERR_TX_FAILED, "upload interrupted");
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        gw_event_ota_t done = {.in_progress = false, .percent = 100, .ok = false};
        gw_event_post(GW_EVENT_OTA, &done, sizeof(done));
        return http_send_error(req, gw_api_err_from_esp(err),
                               err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation"
                                                                  : "esp_ota_end failed");
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return http_send_error(req, gw_api_err_from_esp(err), "could not set boot partition");
    }

    gw_event_ota_t done = {.in_progress = false, .percent = 100, .ok = true};
    gw_event_post(GW_EVENT_OTA, &done, sizeof(done));
    ESP_LOGW(TAG, "OTA written to %s, %u bytes", target->label, (unsigned)received);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", true);
    cJSON_AddStringToObject(res, "next_boot", target->label);
    return http_send_json(req, res);
}

#include <string.h>

#include "gw_api.h"

const char *gw_api_err_str(gw_err_t err)
{
    switch (err) {
        case GW_OK:
            return "ok";
        case GW_ERR_INVALID_ARG:
            return "invalid_arg";
        case GW_ERR_BUS_BUSY:
            return "bus_busy";
        case GW_ERR_BUS_UNPOWERED:
            return "bus_unpowered";
        case GW_ERR_NO_REPLY:
            return "no_reply";
        case GW_ERR_TX_FAILED:
            return "tx_failed";
        case GW_ERR_TIMEOUT:
            return "timeout";
        case GW_ERR_NOT_PRESENT:
            return "not_present";
        case GW_ERR_ADDRESS_IN_USE:
            return "address_in_use";
        case GW_ERR_UNSUPPORTED:
            return "unsupported";
        case GW_ERR_CANCELLED:
            return "cancelled";
        case GW_ERR_INTERNAL:
            return "internal";
    }
    return "internal";
}

gw_err_t gw_api_err_from_esp(esp_err_t err)
{
    switch (err) {
        case ESP_OK:
            return GW_OK;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_INVALID_SIZE:
            return GW_ERR_INVALID_ARG;
        case ESP_ERR_TIMEOUT:
            return GW_ERR_TIMEOUT;
        case ESP_ERR_NOT_FOUND:
            return GW_ERR_NOT_PRESENT;
        case ESP_ERR_NOT_SUPPORTED:
            return GW_ERR_UNSUPPORTED;
        case ESP_ERR_INVALID_RESPONSE:
            return GW_ERR_NO_REPLY;
        // The only NO_MEM a caller can see comes from a full bus queue (dali_bus.h).
        case ESP_ERR_NO_MEM:
            return GW_ERR_BUS_BUSY;
        default:
            return GW_ERR_INTERNAL;
    }
}

void gw_api_result_free(gw_result_t *res)
{
    if (res == NULL || res->data == NULL) {
        return;
    }
    cJSON_Delete(res->data);
    res->data = NULL;
}

cJSON *gw_api_gear_to_json(const gw_gear_t *gear, bool deep)
{
    // TODO(M2): deep blocks (config, identity) land with M3.
    (void)gear;
    (void)deep;
    return NULL;
}

cJSON *gw_api_gears_to_json(const gw_gear_t *gears, size_t count)
{
    // TODO(M2)
    (void)gears;
    (void)count;
    return NULL;
}

cJSON *gw_api_bus_to_json(const gw_bus_status_t *bus)
{
    // TODO(M2)
    (void)bus;
    return NULL;
}

cJSON *gw_api_result_to_json(const gw_result_t *res)
{
    // TODO(M2)
    (void)res;
    return NULL;
}

cJSON *gw_api_progress_to_json(const char *operation, uint16_t done, uint16_t total, uint16_t found)
{
    // TODO(M2)
    (void)operation;
    (void)done;
    (void)total;
    (void)found;
    return NULL;
}

cJSON *gw_api_config_to_json(const app_config_t *cfg, bool include_secrets)
{
    // TODO(M1)
    (void)cfg;
    (void)include_secrets;
    return NULL;
}

esp_err_t gw_api_config_from_json(const cJSON *root, app_config_t *inout, char *err_field,
                                  size_t err_len)
{
    // TODO(M1)
    (void)root;
    (void)inout;
    if (err_field != NULL && err_len > 0) {
        err_field[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_api_cmd_from_json(const char *action, const cJSON *root, gw_cmd_t *out, char *err_msg,
                               size_t err_len)
{
    // TODO(M2)
    (void)action;
    (void)root;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (err_msg != NULL && err_len > 0) {
        err_msg[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gw_api_set_from_payload(const char *payload, size_t len, gw_target_t target,
                                  gw_cmd_t *out, char *err_msg, size_t err_len)
{
    // TODO(M2)
    (void)payload;
    (void)len;
    (void)target;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (err_msg != NULL && err_len > 0) {
        err_msg[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

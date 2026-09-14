#include <string.h>

#include "esp_log.h"

#include "dali_bus.h"

static const char *TAG = "bus";

esp_err_t dali_bus_init(const dali_bus_config_t *cfg)
{
    // TODO(M2): create the queue, the registry and the single bus task that owns the driver.
    (void)cfg;
    ESP_LOGW(TAG, "stub: no bus task, commands are rejected");
    return ESP_OK;
}

esp_err_t dali_bus_submit(const gw_cmd_t *cmd)
{
    // TODO(M2)
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t dali_bus_submit_sync(const gw_cmd_t *cmd, gw_result_t *res, uint32_t timeout_ms)
{
    // TODO(M2)
    (void)cmd;
    (void)timeout_ms;
    if (res != NULL) {
        memset(res, 0, sizeof(*res));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t dali_bus_cancel(void)
{
    // TODO(M2)
    return ESP_ERR_NOT_SUPPORTED;
}

void dali_bus_get_status(gw_bus_status_t *out)
{
    // TODO(M2)
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
}

esp_err_t dali_bus_get_gear(uint8_t addr, gw_gear_t *out)
{
    // TODO(M2)
    if (addr >= GW_MAX_GEARS || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->addr = addr;
    return ESP_OK;
}

esp_err_t dali_bus_get_gears(gw_gear_t *out, size_t max, size_t *count)
{
    // TODO(M2)
    (void)out;
    (void)max;
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    return ESP_OK;
}

esp_err_t dali_bus_set_gear_name(uint8_t addr, const char *name)
{
    // TODO(M3)
    (void)name;
    if (addr >= GW_MAX_GEARS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool dali_bus_indirect_name_valid(const char *name)
{
    // TODO(M2)
    (void)name;
    return false;
}

int16_t dali_bus_query_opcode(const char *name)
{
    // TODO(M2)
    (void)name;
    return -1;
}

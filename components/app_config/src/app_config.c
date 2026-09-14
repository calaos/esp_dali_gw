#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#include "app_config.h"
#include "app_config_logic.h"

static const char *TAG = "cfg";

#define CFG_NVS_NAMESPACE "dali_gw"
#define CFG_NVS_KEY "cfg"

/** Live configuration. Populated by app_config_init(), never NULL to the caller. */
static app_config_t s_cfg;

static esp_err_t store_load(app_config_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(*out);
    err = nvs_get_blob(handle, CFG_NVS_KEY, out, &len);
    nvs_close(handle);
    if (err == ESP_OK && len != sizeof(*out)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t store_save(const app_config_t *cfg)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open");

    /* One blob, one commit. NVS makes a single set+commit atomic, so a power cut mid-write leaves
     * the previous configuration readable instead of a half-updated struct. */
    esp_err_t err = nvs_set_blob(handle, CFG_NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_config_init(void)
{
    app_config_t loaded;
    char field[APP_CONFIG_NAME_LEN] = {0};

    esp_err_t err = store_load(&loaded);
    if (err == ESP_OK) {
        err = app_config_migrate(&loaded);
    }
    if (err == ESP_OK) {
        err = app_config_validate(&loaded, field, sizeof(field));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stored config rejected at '%s'", field);
        }
    }
    if (err == ESP_OK) {
        s_cfg = loaded;
        ESP_LOGI(TAG, "config loaded (schema %u)", (unsigned)s_cfg.schema);
        return ESP_OK;
    }

    app_config_defaults(&s_cfg);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored config, using defaults");
    } else {
        ESP_LOGW(TAG, "config unusable (%s), using defaults", esp_err_to_name(err));
    }
    /* ESP_OK on purpose: refusing to boot on a bad blob would leave the device with no web UI and
     * no way to re-provision it. Nothing is written back either, so the blob stays inspectable. */
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

esp_err_t app_config_set(const app_config_t *cfg, app_config_impact_t *impact)
{
    if (impact != NULL) {
        *impact = APP_CONFIG_IMPACT_NONE;
    }
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_config_validate(cfg, NULL, 0), TAG, "invalid config");

    app_config_t next = *cfg;
    next.schema = APP_CONFIG_SCHEMA_VERSION;
    app_config_impact_t what = app_config_diff_impact(&s_cfg, &next);

    ESP_RETURN_ON_ERROR(store_save(&next), TAG, "persist");
    s_cfg = next;
    if (impact != NULL) {
        *impact = what;
    }
    return ESP_OK;
}

void app_config_merge_secrets(app_config_t *cfg)
{
    app_config_merge_secrets_from(cfg, &s_cfg);
}

esp_err_t app_config_factory_reset(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* already blank */
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    /* s_cfg is left alone and nothing is re-persisted: the caller reboots into provisioning, and
     * a re-write here would recreate the namespace we were asked to erase. */
    return err;
}

void app_config_device_id(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static const char *label_get(const app_config_label_t *label)
{
    return label->name[0] == '\0' ? NULL : label->name;
}

static esp_err_t label_set(app_config_label_t *label, const char *name)
{
    if (name == NULL) {
        name = "";
    }
    if (strlen(name) >= sizeof(label->name)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(label->name, name) == 0) {
        return ESP_OK; /* no flash write for a rename that changes nothing */
    }

    app_config_label_t previous = *label;
    strlcpy(label->name, name, sizeof(label->name));
    esp_err_t err = store_save(&s_cfg);
    if (err != ESP_OK) {
        *label = previous; /* keep RAM and flash telling the same story */
    }
    return err;
}

const char *app_config_gear_name(uint8_t addr)
{
    return addr < APP_CONFIG_MAX_GEARS ? label_get(&s_cfg.gears[addr]) : NULL;
}

const char *app_config_group_name(uint8_t group)
{
    return group < APP_CONFIG_MAX_GROUPS ? label_get(&s_cfg.groups[group]) : NULL;
}

esp_err_t app_config_set_gear_name(uint8_t addr, const char *name)
{
    if (addr >= APP_CONFIG_MAX_GEARS) {
        return ESP_ERR_INVALID_ARG;
    }
    return label_set(&s_cfg.gears[addr], name);
}

esp_err_t app_config_set_group_name(uint8_t group, const char *name)
{
    if (group >= APP_CONFIG_MAX_GROUPS) {
        return ESP_ERR_INVALID_ARG;
    }
    return label_set(&s_cfg.groups[group], name);
}

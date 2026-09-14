#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "cfg";

/** Live configuration. Zeroed until M1 loads the NVS blob, never NULL to the caller. */
static app_config_t s_cfg;

esp_err_t app_config_init(void)
{
    // TODO(M1): open the "dali_gw" namespace, load "cfg", migrate older schemas, fall back to
    // defaults on a corrupt blob. Until then the device always boots on the defaults.
    app_config_defaults(&s_cfg);
    ESP_LOGW(TAG, "stub store: defaults only, nothing is persisted");
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    // TODO(M1)
    return &s_cfg;
}

void app_config_defaults(app_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->schema = APP_CONFIG_SCHEMA_VERSION;

    char id[8];
    app_config_device_id(id, sizeof(id));

    strlcpy(out->device.name, "DALI gateway", sizeof(out->device.name));
    snprintf(out->device.hostname, sizeof(out->device.hostname), "esp-dali-gw-%s", id);
    strlcpy(out->device.timezone, "Europe/Paris", sizeof(out->device.timezone));

    /* An empty ssid is what sends the boot state machine to AP provisioning (SPEC 5.1). */
    strlcpy(out->wifi.ap_password, CONFIG_GW_AP_PASSWORD, sizeof(out->wifi.ap_password));
    out->wifi.fallback_ap_timeout_s = 60;

    out->mqtt.enabled = true;
    snprintf(out->mqtt.client_id, sizeof(out->mqtt.client_id), "esp-dali-gw-%s", id);
    snprintf(out->mqtt.base_topic, sizeof(out->mqtt.base_topic), "dali_gw/%s", id);
    out->mqtt.keepalive_s = 30;
    out->mqtt.qos = 0;
    out->mqtt.retain_state = true;
    strlcpy(out->mqtt.ha_discovery.prefix, "homeassistant", sizeof(out->mqtt.ha_discovery.prefix));

    strlcpy(out->http.auth.username, "admin", sizeof(out->http.auth.username));

    out->dali.tx_gpio = CONFIG_GW_DALI_TX_GPIO;
    out->dali.rx_gpio = CONFIG_GW_DALI_RX_GPIO;
    out->dali.poll_interval_s = 30;
    out->dali.scan_on_boot = true;
    out->dali.identify_blink_ms = 500;

    out->led.enabled = true;
    out->led.gpio = CONFIG_GW_LED_GPIO;
    out->led.brightness = 32;
}

esp_err_t app_config_validate(const app_config_t *cfg, char *err_field, size_t err_len)
{
    // TODO(M1)
    (void)cfg;
    if (err_field != NULL && err_len > 0) {
        err_field[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t app_config_set(const app_config_t *cfg, app_config_impact_t *impact)
{
    // TODO(M1)
    (void)cfg;
    if (impact != NULL) {
        *impact = APP_CONFIG_IMPACT_NONE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void app_config_merge_secrets(app_config_t *cfg)
{
    // TODO(M1)
    (void)cfg;
}

void app_config_mask_secrets(app_config_t *cfg)
{
    // TODO(M1)
    (void)cfg;
}

esp_err_t app_config_factory_reset(void)
{
    // TODO(M1)
    return ESP_ERR_NOT_SUPPORTED;
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

const char *app_config_gear_name(uint8_t addr)
{
    // TODO(M1)
    (void)addr;
    return NULL;
}

const char *app_config_group_name(uint8_t group)
{
    // TODO(M1)
    (void)group;
    return NULL;
}

esp_err_t app_config_set_gear_name(uint8_t addr, const char *name)
{
    // TODO(M1)
    (void)addr;
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_config_set_group_name(uint8_t group, const char *name)
{
    // TODO(M1)
    (void)group;
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
}

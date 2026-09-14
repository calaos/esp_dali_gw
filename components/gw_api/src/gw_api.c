#include <stdio.h>
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

/* --- configuration document (SPEC 6) -------------------------------------------------------- */

static esp_err_t cfg_fail(char *err_field, size_t err_len, const char *path)
{
    if (err_field != NULL && err_len > 0) {
        strlcpy(err_field, path, err_len);
    }
    return ESP_ERR_INVALID_ARG;
}

static void add_secret(cJSON *obj, const char *key, const char *value, bool include_secrets)
{
    cJSON_AddStringToObject(obj, key, include_secrets ? value : APP_CONFIG_SECRET_MASK);
}

/* Named entries only: the document would otherwise carry 64 empty slots on every read. */
static bool add_labels(cJSON *root, const char *key, const app_config_label_t *labels, size_t count)
{
    cJSON *obj = cJSON_AddObjectToObject(root, key);
    if (obj == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (labels[i].name[0] == '\0') {
            continue;
        }
        char addr[4];
        snprintf(addr, sizeof(addr), "%u", (unsigned)i);
        cJSON *entry = cJSON_AddObjectToObject(obj, addr);
        if (entry == NULL || cJSON_AddStringToObject(entry, "name", labels[i].name) == NULL) {
            return false;
        }
    }
    return true;
}

cJSON *gw_api_config_to_json(const app_config_t *cfg, bool include_secrets)
{
    if (cfg == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "schema", cfg->schema);

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "name", cfg->device.name);
    cJSON_AddStringToObject(device, "hostname", cfg->device.hostname);
    cJSON_AddStringToObject(device, "timezone", cfg->device.timezone);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", cfg->wifi.ssid);
    add_secret(wifi, "password", cfg->wifi.password, include_secrets);
    cJSON *static_ip = cJSON_AddObjectToObject(wifi, "static");
    cJSON_AddBoolToObject(static_ip, "enabled", cfg->wifi.static_ip.enabled);
    cJSON_AddStringToObject(static_ip, "ip", cfg->wifi.static_ip.ip);
    cJSON_AddStringToObject(static_ip, "mask", cfg->wifi.static_ip.mask);
    cJSON_AddStringToObject(static_ip, "gw", cfg->wifi.static_ip.gw);
    cJSON_AddStringToObject(static_ip, "dns", cfg->wifi.static_ip.dns);
    add_secret(wifi, "ap_password", cfg->wifi.ap_password, include_secrets);
    cJSON_AddStringToObject(wifi, "country", cfg->wifi.country);
    cJSON_AddNumberToObject(wifi, "fallback_ap_timeout_s", cfg->wifi.fallback_ap_timeout_s);

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", cfg->mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "uri", cfg->mqtt.uri);
    cJSON_AddStringToObject(mqtt, "username", cfg->mqtt.username);
    add_secret(mqtt, "password", cfg->mqtt.password, include_secrets);
    cJSON_AddStringToObject(mqtt, "client_id", cfg->mqtt.client_id);
    cJSON_AddStringToObject(mqtt, "base_topic", cfg->mqtt.base_topic);
    cJSON_AddNumberToObject(mqtt, "keepalive_s", cfg->mqtt.keepalive_s);
    cJSON_AddNumberToObject(mqtt, "qos", cfg->mqtt.qos);
    cJSON_AddBoolToObject(mqtt, "retain_state", cfg->mqtt.retain_state);
    cJSON *ha = cJSON_AddObjectToObject(mqtt, "ha_discovery");
    cJSON_AddBoolToObject(ha, "enabled", cfg->mqtt.ha_discovery.enabled);
    cJSON_AddStringToObject(ha, "prefix", cfg->mqtt.ha_discovery.prefix);

    cJSON *http = cJSON_AddObjectToObject(root, "http");
    cJSON *auth = cJSON_AddObjectToObject(http, "auth");
    cJSON_AddBoolToObject(auth, "enabled", cfg->http.auth.enabled);
    cJSON_AddStringToObject(auth, "username", cfg->http.auth.username);
    add_secret(auth, "password", cfg->http.auth.password, include_secrets);

    cJSON *dali = cJSON_AddObjectToObject(root, "dali");
    cJSON_AddNumberToObject(dali, "tx_gpio", cfg->dali.tx_gpio);
    cJSON_AddNumberToObject(dali, "rx_gpio", cfg->dali.rx_gpio);
    cJSON_AddBoolToObject(dali, "invert_tx", cfg->dali.invert_tx);
    cJSON_AddBoolToObject(dali, "invert_rx", cfg->dali.invert_rx);
    cJSON_AddNumberToObject(dali, "poll_interval_s", cfg->dali.poll_interval_s);
    cJSON_AddBoolToObject(dali, "scan_on_boot", cfg->dali.scan_on_boot);
    cJSON_AddNumberToObject(dali, "identify_blink_ms", cfg->dali.identify_blink_ms);

    cJSON *led = cJSON_AddObjectToObject(root, "led");
    cJSON_AddBoolToObject(led, "enabled", cfg->led.enabled);
    cJSON_AddNumberToObject(led, "gpio", cfg->led.gpio);
    cJSON_AddNumberToObject(led, "brightness", cfg->led.brightness);

    bool labels = add_labels(root, "gears", cfg->gears, APP_CONFIG_MAX_GEARS) &&
                  add_labels(root, "groups", cfg->groups, APP_CONFIG_MAX_GROUPS);

    /* cJSON's add helpers tolerate a NULL parent, so one structural check at the end is enough to
     * turn an allocation failure anywhere above into a NULL return instead of a partial document.
     */
    if (!labels || device == NULL || wifi == NULL || static_ip == NULL || mqtt == NULL ||
        ha == NULL || http == NULL || auth == NULL || dali == NULL || led == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t merge_section(const cJSON *parent, const char *key, const cJSON **out,
                               const char *path, char *err_field, size_t err_len)
{
    *out = NULL;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    if (!cJSON_IsObject(item)) {
        return cfg_fail(err_field, err_len, path);
    }
    *out = item;
    return ESP_OK;
}

static esp_err_t merge_str(const cJSON *parent, const char *key, char *dst, size_t cap,
                           const char *path, char *err_field, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    /* Truncating would store something the client never asked for and that then passes
     * validation, so an oversized string is refused instead. */
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= cap) {
        return cfg_fail(err_field, err_len, path);
    }
    strlcpy(dst, item->valuestring, cap);
    return ESP_OK;
}

/* The mask is what every unauthenticated read returns; writing it back means "keep the stored
 * secret", so it must never reach the config struct. */
static esp_err_t merge_secret(const cJSON *parent, const char *key, char *dst, size_t cap,
                              const char *path, char *err_field, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item != NULL && cJSON_IsString(item) &&
        strcmp(item->valuestring, APP_CONFIG_SECRET_MASK) == 0) {
        return ESP_OK;
    }
    return merge_str(parent, key, dst, cap, path, err_field, err_len);
}

static esp_err_t merge_bool(const cJSON *parent, const char *key, bool *dst, const char *path,
                            char *err_field, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    if (!cJSON_IsBool(item)) {
        return cfg_fail(err_field, err_len, path);
    }
    *dst = cJSON_IsTrue(item);
    return ESP_OK;
}

static esp_err_t merge_num(const cJSON *parent, const char *key, double lo, double hi, double *io,
                           const char *path, char *err_field, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    /* Range and integrality are checked here rather than in app_config_validate because the
     * conversion into the (small, integer) config field would otherwise be undefined. */
    if (!cJSON_IsNumber(item) || item->valuedouble < lo || item->valuedouble > hi ||
        item->valuedouble != (double)(long long)item->valuedouble) {
        return cfg_fail(err_field, err_len, path);
    }
    *io = item->valuedouble;
    return ESP_OK;
}

static bool parse_label_index(const char *key, size_t count, size_t *out)
{
    if (key == NULL || *key == '\0') {
        return false;
    }
    size_t value = 0;
    for (const char *p = key; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (size_t)(*p - '0');
        if (value >= count) {
            return false;
        }
    }
    *out = value;
    return true;
}

static esp_err_t merge_labels(const cJSON *parent, const char *key, app_config_label_t *labels,
                              size_t count, char *err_field, size_t err_len)
{
    const cJSON *obj = NULL;
    esp_err_t err = merge_section(parent, key, &obj, key, err_field, err_len);
    if (err != ESP_OK || obj == NULL) {
        return err;
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, obj)
    {
        char path[48];
        snprintf(path, sizeof(path), "%s.%.16s", key, entry->string != NULL ? entry->string : "");

        size_t index = 0;
        if (!cJSON_IsObject(entry) || !parse_label_index(entry->string, count, &index)) {
            return cfg_fail(err_field, err_len, path);
        }
        size_t used = strlen(path);
        snprintf(path + used, sizeof(path) - used, ".name");
        err = merge_str(entry, "name", labels[index].name, sizeof(labels[index].name), path,
                        err_field, err_len);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* err_field/err_len are captured from gw_api_config_from_json, the only user of these. */
#define CFG_TRY(expr)                                                                              \
    do {                                                                                           \
        esp_err_t cfg_err_ = (expr);                                                               \
        if (cfg_err_ != ESP_OK) {                                                                  \
            return cfg_err_;                                                                       \
        }                                                                                          \
    } while (0)

#define CFG_SECTION(parent, key, out, path)                                                        \
    CFG_TRY(merge_section((parent), (key), &(out), (path), err_field, err_len))
#define CFG_STR(obj, key, field, path)                                                             \
    CFG_TRY(merge_str((obj), (key), (field), sizeof(field), (path), err_field, err_len))
#define CFG_SECRET(obj, key, field, path)                                                          \
    CFG_TRY(merge_secret((obj), (key), (field), sizeof(field), (path), err_field, err_len))
#define CFG_BOOL(obj, key, field, path)                                                            \
    CFG_TRY(merge_bool((obj), (key), &(field), (path), err_field, err_len))
#define CFG_NUM(obj, key, field, lo, hi, path)                                                     \
    do {                                                                                           \
        double cfg_value_ = (double)(field);                                                       \
        CFG_TRY(merge_num((obj), (key), (lo), (hi), &cfg_value_, (path), err_field, err_len));     \
        (field) = (__typeof__(field))cfg_value_;                                                   \
    } while (0)

esp_err_t gw_api_config_from_json(const cJSON *root, app_config_t *inout, char *err_field,
                                  size_t err_len)
{
    if (err_field != NULL && err_len > 0) {
        err_field[0] = '\0';
    }
    if (root == NULL || inout == NULL || !cJSON_IsObject(root)) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *device = NULL;
    const cJSON *wifi = NULL;
    const cJSON *static_ip = NULL;
    const cJSON *mqtt = NULL;
    const cJSON *ha = NULL;
    const cJSON *http = NULL;
    const cJSON *auth = NULL;
    const cJSON *dali = NULL;
    const cJSON *led = NULL;

    CFG_SECTION(root, "device", device, "device");
    CFG_SECTION(root, "wifi", wifi, "wifi");
    CFG_SECTION(wifi, "static", static_ip, "wifi.static");
    CFG_SECTION(root, "mqtt", mqtt, "mqtt");
    CFG_SECTION(mqtt, "ha_discovery", ha, "mqtt.ha_discovery");
    CFG_SECTION(root, "http", http, "http");
    CFG_SECTION(http, "auth", auth, "http.auth");
    CFG_SECTION(root, "dali", dali, "dali");
    CFG_SECTION(root, "led", led, "led");

    CFG_NUM(root, "schema", inout->schema, 0, UINT16_MAX, "schema");

    CFG_STR(device, "name", inout->device.name, "device.name");
    CFG_STR(device, "hostname", inout->device.hostname, "device.hostname");
    CFG_STR(device, "timezone", inout->device.timezone, "device.timezone");

    CFG_STR(wifi, "ssid", inout->wifi.ssid, "wifi.ssid");
    CFG_SECRET(wifi, "password", inout->wifi.password, "wifi.password");
    CFG_SECRET(wifi, "ap_password", inout->wifi.ap_password, "wifi.ap_password");
    CFG_STR(wifi, "country", inout->wifi.country, "wifi.country");
    CFG_NUM(wifi, "fallback_ap_timeout_s", inout->wifi.fallback_ap_timeout_s, 0, UINT16_MAX,
            "wifi.fallback_ap_timeout_s");
    CFG_BOOL(static_ip, "enabled", inout->wifi.static_ip.enabled, "wifi.static.enabled");
    CFG_STR(static_ip, "ip", inout->wifi.static_ip.ip, "wifi.static.ip");
    CFG_STR(static_ip, "mask", inout->wifi.static_ip.mask, "wifi.static.mask");
    CFG_STR(static_ip, "gw", inout->wifi.static_ip.gw, "wifi.static.gw");
    CFG_STR(static_ip, "dns", inout->wifi.static_ip.dns, "wifi.static.dns");

    CFG_BOOL(mqtt, "enabled", inout->mqtt.enabled, "mqtt.enabled");
    CFG_STR(mqtt, "uri", inout->mqtt.uri, "mqtt.uri");
    CFG_STR(mqtt, "username", inout->mqtt.username, "mqtt.username");
    CFG_SECRET(mqtt, "password", inout->mqtt.password, "mqtt.password");
    CFG_STR(mqtt, "client_id", inout->mqtt.client_id, "mqtt.client_id");
    CFG_STR(mqtt, "base_topic", inout->mqtt.base_topic, "mqtt.base_topic");
    CFG_NUM(mqtt, "keepalive_s", inout->mqtt.keepalive_s, 0, UINT16_MAX, "mqtt.keepalive_s");
    CFG_NUM(mqtt, "qos", inout->mqtt.qos, 0, UINT8_MAX, "mqtt.qos");
    CFG_BOOL(mqtt, "retain_state", inout->mqtt.retain_state, "mqtt.retain_state");
    CFG_BOOL(ha, "enabled", inout->mqtt.ha_discovery.enabled, "mqtt.ha_discovery.enabled");
    CFG_STR(ha, "prefix", inout->mqtt.ha_discovery.prefix, "mqtt.ha_discovery.prefix");

    CFG_BOOL(auth, "enabled", inout->http.auth.enabled, "http.auth.enabled");
    CFG_STR(auth, "username", inout->http.auth.username, "http.auth.username");
    CFG_SECRET(auth, "password", inout->http.auth.password, "http.auth.password");

    CFG_NUM(dali, "tx_gpio", inout->dali.tx_gpio, INT8_MIN, INT8_MAX, "dali.tx_gpio");
    CFG_NUM(dali, "rx_gpio", inout->dali.rx_gpio, INT8_MIN, INT8_MAX, "dali.rx_gpio");
    CFG_BOOL(dali, "invert_tx", inout->dali.invert_tx, "dali.invert_tx");
    CFG_BOOL(dali, "invert_rx", inout->dali.invert_rx, "dali.invert_rx");
    CFG_NUM(dali, "poll_interval_s", inout->dali.poll_interval_s, 0, UINT16_MAX,
            "dali.poll_interval_s");
    CFG_BOOL(dali, "scan_on_boot", inout->dali.scan_on_boot, "dali.scan_on_boot");
    CFG_NUM(dali, "identify_blink_ms", inout->dali.identify_blink_ms, 0, UINT16_MAX,
            "dali.identify_blink_ms");

    CFG_BOOL(led, "enabled", inout->led.enabled, "led.enabled");
    CFG_NUM(led, "gpio", inout->led.gpio, INT8_MIN, INT8_MAX, "led.gpio");
    CFG_NUM(led, "brightness", inout->led.brightness, 0, UINT8_MAX, "led.brightness");

    CFG_TRY(merge_labels(root, "gears", inout->gears, APP_CONFIG_MAX_GEARS, err_field, err_len));
    CFG_TRY(merge_labels(root, "groups", inout->groups, APP_CONFIG_MAX_GROUPS, err_field, err_len));
    return ESP_OK;
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

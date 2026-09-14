#include <stdarg.h>
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

/* --- level_pct (SPEC 17) ---------------------------------------------------------------------
 *
 * SPEC 17 left the choice between the DALI logarithmic curve and a linear scale open and proposed
 * linear; linear it is, worded as in SPEC 7.4: 0 is off, 1..100 % spans levels 1..254. Both
 * directions live here so that a level reported to a client and a level_pct accepted from one can
 * never drift apart. The mapping round-trips: pct -> level -> pct is the identity over 0..100.
 */
#define GW_LEVEL_MAX 254
#define GW_PCT_MAX 100

static uint8_t level_to_pct(uint8_t level)
{
    if (level == 0) {
        return 0;
    }
    /* 255 is MASK ("no change"), not a level; treat it as full scale rather than overflowing. */
    if (level > GW_LEVEL_MAX) {
        level = GW_LEVEL_MAX;
    }
    int span = GW_LEVEL_MAX - 1;
    return (uint8_t)(1 + ((level - 1) * (GW_PCT_MAX - 1) + span / 2) / span);
}

static uint8_t level_from_pct(uint8_t pct)
{
    if (pct == 0) {
        return 0;
    }
    if (pct > GW_PCT_MAX) {
        pct = GW_PCT_MAX;
    }
    int span = GW_PCT_MAX - 1;
    return (uint8_t)(1 + ((pct - 1) * (GW_LEVEL_MAX - 1) + span / 2) / span);
}

/* --- serialization: gateway to client ------------------------------------------------------- */

static const char *target_type_str(gw_target_type_t type)
{
    switch (type) {
        case GW_TARGET_GROUP:
            return "group";
        case GW_TARGET_BROADCAST:
            return "broadcast";
        case GW_TARGET_SHORT:
            break;
    }
    return "short";
}

static void add_num_or_null(cJSON *obj, const char *key, bool valid, double value)
{
    if (valid) {
        cJSON_AddNumberToObject(obj, key, value);
    } else {
        cJSON_AddNullToObject(obj, key);
    }
}

/* A gear that has never answered QUERY ACTUAL LEVEL reports null, not 0: a client has to be able
 * to tell "not read yet" from "off". */
static void add_level(cJSON *obj, const gw_gear_t *gear, bool with_pct)
{
    add_num_or_null(obj, "level", gear->level_valid, gear->level);
    if (with_pct) {
        add_num_or_null(obj, "level_pct", gear->level_valid, level_to_pct(gear->level));
    }
    if (gear->level_valid) {
        cJSON_AddBoolToObject(obj, "on", gear->level > 0);
    } else {
        cJSON_AddNullToObject(obj, "on");
    }
}

/* An unanswered QUERY STATUS carries no bits; emitting eight false ones would read as a healthy
 * gear, so only `raw` is published and it is null. */
static cJSON *add_status(cJSON *parent, const gw_gear_status_t *status, bool decoded)
{
    cJSON *obj = cJSON_AddObjectToObject(parent, "status");
    if (obj == NULL) {
        return NULL;
    }
    add_num_or_null(obj, "raw", status->valid, status->raw);
    if (!decoded || !status->valid) {
        return obj;
    }
    cJSON_AddBoolToObject(obj, "gear_failure", status->gear_failure);
    cJSON_AddBoolToObject(obj, "lamp_failure", status->lamp_failure);
    cJSON_AddBoolToObject(obj, "lamp_on", status->lamp_on);
    cJSON_AddBoolToObject(obj, "limit_error", status->limit_error);
    cJSON_AddBoolToObject(obj, "fade_running", status->fade_running);
    cJSON_AddBoolToObject(obj, "reset_state", status->reset_state);
    cJSON_AddBoolToObject(obj, "missing_short_address", status->missing_short_address);
    cJSON_AddBoolToObject(obj, "power_failure", status->power_failure);
    return obj;
}

/* The wire form is the list of groups the gear belongs to, not the membership word (SPEC 7.3). */
static bool add_groups(cJSON *parent, uint16_t mask)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, "groups");
    if (arr == NULL) {
        return false;
    }
    for (unsigned n = 0; n < GW_MAX_GROUPS; n++) {
        if ((mask & (1u << n)) == 0) {
            continue;
        }
        cJSON *item = cJSON_CreateNumber(n);
        if (item == NULL || !cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            return false;
        }
    }
    return true;
}

static bool add_scenes(cJSON *parent, const int16_t *scenes)
{
    cJSON *arr = cJSON_AddArrayToObject(parent, "scenes");
    if (arr == NULL) {
        return false;
    }
    for (size_t i = 0; i < GW_MAX_SCENES; i++) {
        /* 0xFF means "scene not programmed" and is not level 255, so it is null (SPEC 7.3). */
        cJSON *item = scenes[i] < 0 ? cJSON_CreateNull() : cJSON_CreateNumber(scenes[i]);
        if (item == NULL || !cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            return false;
        }
    }
    return true;
}

static bool add_gear_config(cJSON *parent, const gw_gear_config_t *cfg)
{
    cJSON *obj = cJSON_AddObjectToObject(parent, "config");
    if (obj == NULL) {
        return false;
    }
    cJSON_AddNumberToObject(obj, "min", cfg->min);
    cJSON_AddNumberToObject(obj, "max", cfg->max);
    cJSON_AddNumberToObject(obj, "power_on", cfg->power_on);
    cJSON_AddNumberToObject(obj, "system_failure", cfg->system_failure);
    cJSON_AddNumberToObject(obj, "fade_time", cfg->fade_time);
    cJSON_AddNumberToObject(obj, "fade_rate", cfg->fade_rate);
    cJSON_AddNumberToObject(obj, "physical_min", cfg->physical_min);
    return add_groups(obj, cfg->groups) && add_scenes(obj, cfg->scenes);
}

static bool add_gear_identity(cJSON *parent, const gw_gear_identity_t *id)
{
    cJSON *obj = cJSON_AddObjectToObject(parent, "identity");
    if (obj == NULL) {
        return false;
    }
    cJSON_AddStringToObject(obj, "gtin", id->gtin);
    cJSON_AddStringToObject(obj, "serial", id->serial);
    cJSON_AddNumberToObject(obj, "bank0_version", id->bank0_version);
    return true;
}

cJSON *gw_api_gear_to_json(const gw_gear_t *gear, bool deep)
{
    if (gear == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(obj, "addr", gear->addr);
    cJSON_AddStringToObject(obj, "name", gear->name);
    cJSON_AddBoolToObject(obj, "present", gear->present);
    cJSON_AddNumberToObject(obj, "last_seen", (double)gear->last_seen);

    cJSON *types = cJSON_AddArrayToObject(obj, "device_types");
    size_t type_count = gear->device_type_count;
    if (type_count > sizeof(gear->device_types)) {
        type_count = sizeof(gear->device_types);
    }
    for (size_t i = 0; types != NULL && i < type_count; i++) {
        cJSON *item = cJSON_CreateNumber(gear->device_types[i]);
        if (item == NULL || !cJSON_AddItemToArray(types, item)) {
            cJSON_Delete(item);
            types = NULL;
        }
    }

    char version[8];
    snprintf(version, sizeof(version), "%u.%u", (unsigned)gear->version_major,
             (unsigned)gear->version_minor);
    cJSON_AddStringToObject(obj, "version", version);

    cJSON *dt8 = NULL;
    if (gear->dt8.supported) {
        dt8 = cJSON_AddObjectToObject(obj, "dt8");
        cJSON_AddNumberToObject(dt8, "caps", gear->dt8.caps);
        cJSON_AddNumberToObject(dt8, "tc_min", gear->dt8.tc_min_mirek);
        cJSON_AddNumberToObject(dt8, "tc_max", gear->dt8.tc_max_mirek);
    }

    add_level(obj, gear, true);
    bool ok = add_status(obj, &gear->status, true) != NULL;

    /* Blocks that were never read are absent rather than zero-filled, so a client can tell an
     * unscanned gear from one whose min level really is 0. */
    if (deep && gear->config.valid) {
        ok = ok && add_gear_config(obj, &gear->config);
    }
    if (deep && gear->identity.valid) {
        ok = ok && add_gear_identity(obj, &gear->identity);
    }

    if (!ok || types == NULL || (gear->dt8.supported && dt8 == NULL)) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

cJSON *gw_api_gears_to_json(const gw_gear_t *gears, size_t count)
{
    if (gears == NULL && count > 0) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "gears");
    if (arr == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL || !cJSON_AddItemToArray(arr, entry)) {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(entry, "addr", gears[i].addr);
        cJSON_AddStringToObject(entry, "name", gears[i].name);
        cJSON_AddBoolToObject(entry, "present", gears[i].present);
        add_level(entry, &gears[i], false);
        if (add_status(entry, &gears[i].status, false) == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

cJSON *gw_api_bus_to_json(const gw_bus_status_t *bus)
{
    if (bus == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(obj, "powered", bus->powered);
    cJSON_AddBoolToObject(obj, "busy", bus->busy);

    /* null rather than "": clients test for the absence of an operation, never for an empty name.
     */
    if (bus->operation[0] == '\0') {
        cJSON_AddNullToObject(obj, "operation");
    } else {
        cJSON_AddStringToObject(obj, "operation", bus->operation);
    }

    cJSON *progress = NULL;
    if (bus->has_progress) {
        progress = cJSON_AddObjectToObject(obj, "progress");
        cJSON_AddNumberToObject(progress, "done", bus->done);
        cJSON_AddNumberToObject(progress, "total", bus->total);
        cJSON_AddNumberToObject(progress, "found", bus->found);
    } else {
        cJSON_AddNullToObject(obj, "progress");
    }

    cJSON_AddNumberToObject(obj, "gear_count", bus->gear_count);
    cJSON_AddNumberToObject(obj, "last_scan", (double)bus->last_scan);

    if (bus->has_progress && progress == NULL) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

cJSON *gw_api_result_to_json(const gw_result_t *res)
{
    if (res == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(obj, "id", res->id);
    cJSON_AddStringToObject(obj, "action", res->action);
    cJSON_AddBoolToObject(obj, "ok", res->ok);
    cJSON_AddNumberToObject(obj, "duration_ms", res->duration_ms);

    bool ok = true;
    if (res->ok) {
        /* The result keeps ownership of `data`: the parameter is const, so this cannot detach it,
         * and gw_api_result_free() still frees it afterwards. Hence a deep copy. */
        if (res->data != NULL) {
            cJSON *copy = cJSON_Duplicate(res->data, true);
            ok = copy != NULL && cJSON_AddItemToObject(obj, "data", copy);
            if (!ok) {
                cJSON_Delete(copy);
            }
        }
    } else {
        cJSON_AddStringToObject(obj, "error", gw_api_err_str(res->error));
        cJSON_AddStringToObject(obj, "message", res->message);
    }

    if (res->has_target) {
        cJSON *target = cJSON_AddObjectToObject(obj, "target");
        cJSON_AddStringToObject(target, "type", target_type_str(res->target.type));
        /* A broadcast has no address; a 0 here would read as short address 0. */
        if (res->target.type != GW_TARGET_BROADCAST) {
            cJSON_AddNumberToObject(target, "addr", res->target.addr);
        }
        ok = ok && target != NULL;
    }

    if (!ok) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

cJSON *gw_api_progress_to_json(const char *operation, uint16_t done, uint16_t total, uint16_t found)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "operation", operation != NULL ? operation : "");
    cJSON_AddNumberToObject(obj, "done", done);
    cJSON_AddNumberToObject(obj, "total", total);
    cJSON_AddNumberToObject(obj, "found", found);
    return obj;
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
    if (include_secrets) {
        cJSON_AddStringToObject(obj, key, value);
        return;
    }
    /*
     * An empty stored secret is reported as empty, not masked. Masking it would tell a client that
     * a password exists when none does, and the UI would offer to keep a value that is not there.
     * An empty string is not a secret, so nothing leaks by saying so.
     */
    cJSON_AddStringToObject(obj, key, value[0] != '\0' ? APP_CONFIG_SECRET_MASK : "");
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

/* --- deserialization: client to gateway ----------------------------------------------------- */

__attribute__((format(printf, 3, 4))) static esp_err_t cmd_fail(char *err_msg, size_t err_len,
                                                                const char *fmt, ...)
{
    if (err_msg != NULL && err_len > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err_msg, err_len, fmt, ap);
        va_end(ap);
    }
    return ESP_ERR_INVALID_ARG;
}

#define CMD_TRY(expr)                                                                              \
    do {                                                                                           \
        esp_err_t cmd_err_ = (expr);                                                               \
        if (cmd_err_ != ESP_OK) {                                                                  \
            return cmd_err_;                                                                       \
        }                                                                                          \
    } while (0)

/* Absent and malformed have to stay distinguishable: every member of a command payload is
 * optional to *some* action, but a present-and-wrong one is always a client bug. */
typedef enum {
    FIELD_ABSENT = 0,
    FIELD_OK,
    FIELD_BAD,
} field_state_t;

static bool num_value(const cJSON *item, long lo, long hi, long *out)
{
    /* JSON has one number type, so integrality is checked here or the narrowing into the uint8_t
     * command fields would be undefined. */
    if (!cJSON_IsNumber(item) || item->valuedouble < (double)lo || item->valuedouble > (double)hi ||
        item->valuedouble != (double)(long long)item->valuedouble) {
        return false;
    }
    *out = (long)item->valuedouble;
    return true;
}

static field_state_t read_num(const cJSON *root, const char *key, long lo, long hi, long *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return FIELD_ABSENT;
    }
    return num_value(item, lo, hi, out) ? FIELD_OK : FIELD_BAD;
}

static field_state_t read_bool(const cJSON *root, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return FIELD_ABSENT;
    }
    if (!cJSON_IsBool(item)) {
        return FIELD_BAD;
    }
    *out = cJSON_IsTrue(item);
    return FIELD_OK;
}

static field_state_t read_str(const cJSON *root, const char *key, char *dst, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL || cJSON_IsNull(item)) {
        return FIELD_ABSENT;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL || strlen(item->valuestring) >= cap) {
        return FIELD_BAD;
    }
    strlcpy(dst, item->valuestring, cap);
    return FIELD_OK;
}

/* SPEC 8 allows "id" to be a string, but it is echoed back through a uint32_t, so a non-numeric
 * one could never reach the client again: refusing it beats silently answering with id 0. */
static esp_err_t parse_id(const cJSON *root, uint32_t *out, char *err_msg, size_t err_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (item == NULL || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    if (cJSON_IsNumber(item)) {
        long value = 0;
        if (!num_value(item, 0, (long)UINT32_MAX, &value)) {
            return cmd_fail(err_msg, err_len, "id must be a whole number 0..4294967295");
        }
        *out = (uint32_t)value;
        return ESP_OK;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        uint64_t value = 0;
        for (const char *p = item->valuestring; *p != '\0'; p++) {
            if (*p < '0' || *p > '9' || value > UINT32_MAX) {
                return cmd_fail(err_msg, err_len, "id must be a number or a numeric string");
            }
            value = value * 10 + (uint64_t)(*p - '0');
        }
        if (value > UINT32_MAX) {
            return cmd_fail(err_msg, err_len, "id must be a number or a numeric string");
        }
        *out = (uint32_t)value;
        return ESP_OK;
    }
    return cmd_fail(err_msg, err_len, "id must be a number or a numeric string");
}

typedef enum {
    TARGET_ANY = 0,
    TARGET_SHORT_ONLY, /**< re-addressing needs one gear, not a group */
} target_mode_t;

static esp_err_t parse_target(const cJSON *root, target_mode_t mode, gw_target_t *out,
                              char *err_msg, size_t err_len)
{
    long value = 0;
    switch (read_num(root, "addr", 0, GW_MAX_GEARS - 1, &value)) {
        case FIELD_OK:
            out->type = GW_TARGET_SHORT;
            out->addr = (uint8_t)value;
            return ESP_OK;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "addr must be a short address 0..63");
        case FIELD_ABSENT:
            break;
    }
    if (mode == TARGET_SHORT_ONLY) {
        return cmd_fail(err_msg, err_len, "addr is required");
    }
    switch (read_num(root, "group", 0, GW_MAX_GROUPS - 1, &value)) {
        case FIELD_OK:
            out->type = GW_TARGET_GROUP;
            out->addr = (uint8_t)value;
            return ESP_OK;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "group must be 0..15");
        case FIELD_ABSENT:
            break;
    }
    bool broadcast = false;
    switch (read_bool(root, "broadcast", &broadcast)) {
        case FIELD_OK:
            if (broadcast) {
                out->type = GW_TARGET_BROADCAST;
                out->addr = 0;
                return ESP_OK;
            }
            break;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "broadcast must be a boolean");
        case FIELD_ABSENT:
            break;
    }
    return cmd_fail(err_msg, err_len, "needs addr, group or broadcast");
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t parse_raw(const cJSON *root, gw_cmd_t *cmd, char *err_msg, size_t err_len)
{
    char text[8];
    if (read_str(root, "frame", text, sizeof(text)) != FIELD_OK) {
        return cmd_fail(err_msg, err_len, "frame must be a string of 4 or 6 hex digits");
    }
    /* Only whole 16- and 24-bit frames: any other length would shift the opcode into the address
     * byte instead of being rejected. */
    size_t digits = strlen(text);
    if (digits != 4 && digits != 6) {
        return cmd_fail(err_msg, err_len, "frame must be 4 or 6 hex digits, got %u",
                        (unsigned)digits);
    }
    uint32_t frame = 0;
    for (size_t i = 0; i < digits; i++) {
        int nibble = hex_digit(text[i]);
        if (nibble < 0) {
            return cmd_fail(err_msg, err_len, "frame is not hexadecimal");
        }
        frame = (frame << 4) | (uint32_t)nibble;
    }
    cmd->args.raw.frame = frame;
    cmd->args.raw.bits = (uint8_t)(digits * 4);
    if (read_bool(root, "send_twice", &cmd->args.raw.send_twice) == FIELD_BAD) {
        return cmd_fail(err_msg, err_len, "send_twice must be a boolean");
    }
    if (read_bool(root, "expect_reply", &cmd->args.raw.expect_reply) == FIELD_BAD) {
        return cmd_fail(err_msg, err_len, "expect_reply must be a boolean");
    }
    return ESP_OK;
}

static bool parse_index(const char *key, long limit, long *out)
{
    if (key == NULL || *key == '\0') {
        return false;
    }
    long value = 0;
    for (const char *p = key; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (*p - '0');
        if (value >= limit) {
            return false;
        }
    }
    *out = value;
    return true;
}

static esp_err_t parse_scene_map(const cJSON *root, gw_configure_args_t *cfg, char *err_msg,
                                 size_t err_len)
{
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(root, "scene");
    if (map == NULL || cJSON_IsNull(map)) {
        return ESP_OK;
    }
    if (!cJSON_IsObject(map)) {
        return cmd_fail(err_msg, err_len, "scene must be an object keyed by scene number");
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, map)
    {
        long index = 0;
        if (!parse_index(entry->string, GW_MAX_SCENES, &index)) {
            return cmd_fail(err_msg, err_len, "scene keys must be 0..%d", GW_MAX_SCENES - 1);
        }
        long level = 0;
        if (cJSON_IsNull(entry)) {
            cfg->scenes[index] = GW_SCENE_CLEAR;
        } else if (num_value(entry, 0, GW_LEVEL_MAX, &level)) {
            cfg->scenes[index] = (int16_t)level;
        } else {
            return cmd_fail(err_msg, err_len, "scene %ld must be 0..%d or null", index,
                            GW_LEVEL_MAX);
        }
        cfg->fields |= GW_CFG_FIELD_SCENES;
    }
    return ESP_OK;
}

static esp_err_t parse_group_mask(const cJSON *parent, const char *key, uint16_t *mask,
                                  char *err_msg, size_t err_len)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (arr == NULL || cJSON_IsNull(arr)) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(arr)) {
        return cmd_fail(err_msg, err_len, "group.%s must be an array", key);
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        long group = 0;
        if (!num_value(item, 0, GW_MAX_GROUPS - 1, &group)) {
            return cmd_fail(err_msg, err_len, "group.%s must hold groups 0..%d", key,
                            GW_MAX_GROUPS - 1);
        }
        *mask |= (uint16_t)(1u << (unsigned)group);
    }
    return ESP_OK;
}

static esp_err_t parse_groups(const cJSON *root, gw_configure_args_t *cfg, char *err_msg,
                              size_t err_len)
{
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group");
    if (group == NULL || cJSON_IsNull(group)) {
        return ESP_OK;
    }
    if (!cJSON_IsObject(group)) {
        return cmd_fail(err_msg, err_len, "group must be an object with add and remove");
    }
    CMD_TRY(parse_group_mask(group, "add", &cfg->group_add, err_msg, err_len));
    CMD_TRY(parse_group_mask(group, "remove", &cfg->group_remove, err_msg, err_len));
    /* Joining and leaving the same group in one command has no defined outcome on the bus. */
    if ((cfg->group_add & cfg->group_remove) != 0) {
        return cmd_fail(err_msg, err_len, "a group cannot be in both add and remove");
    }
    if ((cfg->group_add | cfg->group_remove) != 0) {
        cfg->fields |= GW_CFG_FIELD_GROUPS;
    }
    return ESP_OK;
}

#define CFG_FIELD(key, member, flag, lo, hi)                                                       \
    do {                                                                                           \
        long v_ = 0;                                                                               \
        switch (read_num(root, (key), (lo), (hi), &v_)) {                                          \
            case FIELD_OK:                                                                         \
                cfg->member = (uint8_t)v_;                                                         \
                cfg->fields |= (flag);                                                             \
                break;                                                                             \
            case FIELD_BAD:                                                                        \
                return cmd_fail(err_msg, err_len, "%s must be %d..%d", (key), (int)(lo),           \
                                (int)(hi));                                                        \
            case FIELD_ABSENT:                                                                     \
                break;                                                                             \
        }                                                                                          \
    } while (0)

static esp_err_t parse_configure(const cJSON *root, gw_configure_args_t *cfg, char *err_msg,
                                 size_t err_len)
{
    for (size_t i = 0; i < GW_MAX_SCENES; i++) {
        cfg->scenes[i] = GW_SCENE_UNTOUCHED;
    }
    CFG_FIELD("min", min, GW_CFG_FIELD_MIN, 0, GW_LEVEL_MAX);
    CFG_FIELD("max", max, GW_CFG_FIELD_MAX, 0, GW_LEVEL_MAX);
    /* 255 is MASK on these two and means "keep the last active level", a legal stored value. */
    CFG_FIELD("power_on", power_on, GW_CFG_FIELD_POWER_ON, 0, 255);
    CFG_FIELD("system_failure", system_failure, GW_CFG_FIELD_SYSTEM_FAILURE, 0, 255);
    CFG_FIELD("fade_time", fade_time, GW_CFG_FIELD_FADE_TIME, 0, 15);
    CFG_FIELD("fade_rate", fade_rate, GW_CFG_FIELD_FADE_RATE, 1, 15);
    CFG_FIELD("extended_fade_time", ext_fade_time, GW_CFG_FIELD_EXT_FADE_TIME, 0, 255);
    CMD_TRY(parse_scene_map(root, cfg, err_msg, err_len));
    CMD_TRY(parse_groups(root, cfg, err_msg, err_len));
    if (cfg->fields == 0) {
        return cmd_fail(err_msg, err_len, "configure needs at least one parameter");
    }
    return ESP_OK;
}

static esp_err_t parse_commission(const cJSON *root, gw_cmd_t *cmd, char *err_msg, size_t err_len)
{
    char mode[16] = "unaddressed";
    if (read_str(root, "mode", mode, sizeof(mode)) == FIELD_BAD) {
        return cmd_fail(err_msg, err_len, "mode must be \"all\" or \"unaddressed\"");
    }
    if (strcmp(mode, "all") == 0) {
        cmd->args.commission.mode = GW_COMMISSION_ALL;
    } else if (strcmp(mode, "unaddressed") == 0) {
        cmd->args.commission.mode = GW_COMMISSION_UNADDRESSED;
    } else {
        return cmd_fail(err_msg, err_len, "mode must be \"all\" or \"unaddressed\"");
    }
    if (read_bool(root, "confirm", &cmd->args.commission.confirm) == FIELD_BAD) {
        return cmd_fail(err_msg, err_len, "confirm must be a boolean");
    }
    long start = 0;
    switch (read_num(root, "start_addr", 0, GW_MAX_GEARS - 1, &start)) {
        case FIELD_OK:
            cmd->args.commission.start_addr = (uint8_t)start;
            break;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "start_addr must be 0..63");
        case FIELD_ABSENT:
            break;
    }
    /* SPEC 17: "all" renumbers every gear on the bus, so a stray retained MQTT message must not be
     * able to trigger it. */
    if (cmd->args.commission.mode == GW_COMMISSION_ALL && !cmd->args.commission.confirm) {
        return cmd_fail(err_msg, err_len,
                        "mode \"all\" re-addresses every gear and needs \"confirm\": true");
    }
    return ESP_OK;
}

static esp_err_t parse_query(const cJSON *root, gw_cmd_t *cmd, char *err_msg, size_t err_len)
{
    /* Query names are resolved in dali_bus; gw_api knows no opcodes (ADR 0002). */
    cmd->args.query.opcode = -1;
    bool has_name = false;
    switch (read_str(root, "query", cmd->args.query.name, sizeof(cmd->args.query.name))) {
        case FIELD_OK:
            has_name = cmd->args.query.name[0] != '\0';
            break;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "query must be a name of at most %d characters",
                            GW_CMDNAME_LEN - 1);
        case FIELD_ABSENT:
            break;
    }
    long opcode = 0;
    bool has_opcode = false;
    switch (read_num(root, "opcode", 0, 255, &opcode)) {
        case FIELD_OK:
            has_opcode = true;
            break;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "opcode must be 0..255");
        case FIELD_ABSENT:
            break;
    }
    if (has_name == has_opcode) {
        return cmd_fail(err_msg, err_len, "give either a query name or an opcode, not both");
    }
    if (has_opcode) {
        cmd->args.query.name[0] = '\0';
        cmd->args.query.opcode = (int16_t)opcode;
    }
    return ESP_OK;
}

esp_err_t gw_api_cmd_from_json(const char *action, const cJSON *root, gw_cmd_t *out, char *err_msg,
                               size_t err_len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (err_msg != NULL && err_len > 0) {
        err_msg[0] = '\0';
    }
    if (action == NULL) {
        return cmd_fail(err_msg, err_len, "missing action");
    }
    if (root != NULL && !cJSON_IsObject(root)) {
        return cmd_fail(err_msg, err_len, "payload must be a JSON object");
    }

    gw_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    CMD_TRY(parse_id(root, &cmd.id, err_msg, err_len));

    if (strcmp(action, "scan") == 0) {
        cmd.kind = GW_CMD_SCAN;
        if (read_bool(root, "deep", &cmd.args.scan.deep) == FIELD_BAD) {
            return cmd_fail(err_msg, err_len, "deep must be a boolean");
        }
    } else if (strcmp(action, "commission") == 0) {
        cmd.kind = GW_CMD_COMMISSION;
        CMD_TRY(parse_commission(root, &cmd, err_msg, err_len));
    } else if (strcmp(action, "cancel") == 0) {
        /* Pointless behind the long operation it is meant to stop (SPEC 7.2). */
        cmd.kind = GW_CMD_CANCEL;
        cmd.urgent = true;
    } else if (strcmp(action, "query") == 0) {
        CMD_TRY(parse_target(root, TARGET_ANY, &cmd.target, err_msg, err_len));
        cmd.kind = GW_CMD_QUERY;
        CMD_TRY(parse_query(root, &cmd, err_msg, err_len));
    } else if (strcmp(action, "configure") == 0) {
        CMD_TRY(parse_target(root, TARGET_ANY, &cmd.target, err_msg, err_len));
        cmd.kind = GW_CMD_CONFIGURE;
        CMD_TRY(parse_configure(root, &cmd.args.configure, err_msg, err_len));
    } else if (strcmp(action, "set_short_address") == 0) {
        CMD_TRY(parse_target(root, TARGET_SHORT_ONLY, &cmd.target, err_msg, err_len));
        cmd.kind = GW_CMD_SET_SHORT_ADDRESS;
        long new_addr = 0;
        if (read_num(root, "new_addr", 0, GW_MAX_GEARS - 1, &new_addr) != FIELD_OK) {
            return cmd_fail(err_msg, err_len, "new_addr must be 0..63");
        }
        cmd.args.set_short_address.new_addr = (uint8_t)new_addr;
    } else if (strcmp(action, "remove_short_address") == 0) {
        CMD_TRY(parse_target(root, TARGET_SHORT_ONLY, &cmd.target, err_msg, err_len));
        cmd.kind = GW_CMD_REMOVE_SHORT_ADDRESS;
    } else if (strcmp(action, "identify") == 0) {
        CMD_TRY(parse_target(root, TARGET_ANY, &cmd.target, err_msg, err_len));
        cmd.kind = GW_CMD_IDENTIFY;
    } else if (strcmp(action, "raw") == 0) {
        /* A raw frame carries its own addressing; the target only labels the echoed result. */
        cmd.target.type = GW_TARGET_BROADCAST;
        cmd.kind = GW_CMD_RAW;
        CMD_TRY(parse_raw(root, &cmd, err_msg, err_len));
    } else if (strcmp(action, "bus_check") == 0) {
        cmd.target.type = GW_TARGET_BROADCAST;
        cmd.kind = GW_CMD_BUS_CHECK;
    } else if (strcmp(action, "poll_all") == 0) {
        cmd.kind = GW_CMD_POLL_ALL;
    } else {
        return cmd_fail(err_msg, err_len, "unknown action '%.24s'", action);
    }

    *out = cmd;
    return ESP_OK;
}

/* --- set payloads ----------------------------------------------------------------------------- */

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool token_equals(const char *text, size_t len, const char *literal)
{
    if (len != strlen(literal)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != literal[i]) {
            return false;
        }
    }
    return true;
}

static esp_err_t parse_indirect(const char *text, gw_cmd_t *cmd, char *err_msg, size_t err_len)
{
    const char *colon = strchr(text, ':');
    size_t name_len = colon != NULL ? (size_t)(colon - text) : strlen(text);
    if (name_len == 0 || name_len >= GW_CMDNAME_LEN) {
        return cmd_fail(err_msg, err_len, "cmd must be a name of at most %d characters",
                        GW_CMDNAME_LEN - 1);
    }
    memcpy(cmd->args.indirect.name, text, name_len);
    cmd->args.indirect.name[name_len] = '\0';

    /* "go_to_scene:3" is the form SPEC 7.4 documents; the scene travels beside the name because
     * the opcode table in dali_bus indexes it. */
    if (colon != NULL) {
        long scene = 0;
        if (!parse_index(colon + 1, GW_MAX_SCENES, &scene)) {
            return cmd_fail(err_msg, err_len, "scene after ':' must be 0..%d", GW_MAX_SCENES - 1);
        }
        cmd->args.indirect.scene = (uint8_t)scene;
    } else if (strcmp(cmd->args.indirect.name, "go_to_scene") == 0) {
        return cmd_fail(err_msg, err_len, "go_to_scene needs a scene number, e.g. go_to_scene:3");
    }
    cmd->kind = GW_CMD_INDIRECT;
    return ESP_OK;
}

static esp_err_t parse_channels(const cJSON *arr, size_t want, uint8_t *dst, const char *key,
                                char *err_msg, size_t err_len)
{
    if (!cJSON_IsArray(arr) || (size_t)cJSON_GetArraySize(arr) != want) {
        return cmd_fail(err_msg, err_len, "%s must be an array of %u values 0..%d", key,
                        (unsigned)want, GW_LEVEL_MAX);
    }
    size_t i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        long value = 0;
        if (!num_value(item, 0, GW_LEVEL_MAX, &value)) {
            return cmd_fail(err_msg, err_len, "%s must be an array of %u values 0..%d", key,
                            (unsigned)want, GW_LEVEL_MAX);
        }
        dst[i++] = (uint8_t)value;
    }
    return ESP_OK;
}

static esp_err_t parse_color(const cJSON *root, gw_cmd_t *cmd, bool *handled, char *err_msg,
                             size_t err_len)
{
    long value = 0;
    switch (read_num(root, "mirek", 1, UINT16_MAX, &value)) {
        case FIELD_OK:
            cmd->kind = GW_CMD_COLOR;
            cmd->args.color.kind = GW_COLOR_MIREK;
            cmd->args.color.mirek = (uint16_t)value;
            *handled = true;
            return ESP_OK;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "mirek must be 1..65535");
        case FIELD_ABSENT:
            break;
    }
    /* The lower bound is the divide-by-zero guard: mirek = 1e6 / K. */
    switch (read_num(root, "kelvin", 1, 1000000, &value)) {
        case FIELD_OK: {
            long mirek = 1000000 / value;
            cmd->kind = GW_CMD_COLOR;
            cmd->args.color.kind = GW_COLOR_MIREK;
            cmd->args.color.mirek = (uint16_t)(mirek > UINT16_MAX ? UINT16_MAX : mirek);
            *handled = true;
            return ESP_OK;
        }
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "kelvin must be 1..1000000");
        case FIELD_ABSENT:
            break;
    }
    const cJSON *rgb = cJSON_GetObjectItemCaseSensitive(root, "rgb");
    if (rgb != NULL && !cJSON_IsNull(rgb)) {
        CMD_TRY(parse_channels(rgb, 3, cmd->args.color.channels, "rgb", err_msg, err_len));
        cmd->kind = GW_CMD_COLOR;
        cmd->args.color.kind = GW_COLOR_RGB;
        *handled = true;
        return ESP_OK;
    }
    const cJSON *rgbwaf = cJSON_GetObjectItemCaseSensitive(root, "rgbwaf");
    if (rgbwaf != NULL && !cJSON_IsNull(rgbwaf)) {
        CMD_TRY(parse_channels(rgbwaf, 6, cmd->args.color.channels, "rgbwaf", err_msg, err_len));
        cmd->kind = GW_CMD_COLOR;
        cmd->args.color.kind = GW_COLOR_RGBWAF;
        *handled = true;
    }
    return ESP_OK;
}

static esp_err_t set_from_json(const cJSON *root, gw_cmd_t *cmd, char *err_msg, size_t err_len)
{
    long value = 0;
    if (cJSON_IsNumber(root)) {
        if (!num_value(root, 0, GW_LEVEL_MAX, &value)) {
            return cmd_fail(err_msg, err_len, "a bare level must be 0..%d", GW_LEVEL_MAX);
        }
        cmd->kind = GW_CMD_SET_LEVEL;
        cmd->args.set_level.level = (uint8_t)value;
        cmd->urgent = true;
        return ESP_OK;
    }
    if (!cJSON_IsObject(root)) {
        return cmd_fail(err_msg, err_len, "payload must be an object, a level, ON or OFF");
    }

    bool has_level = false;
    switch (read_num(root, "level", 0, GW_LEVEL_MAX, &value)) {
        case FIELD_OK:
            cmd->args.set_level.level = (uint8_t)value;
            has_level = true;
            break;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "level must be 0..%d", GW_LEVEL_MAX);
        case FIELD_ABSENT:
            break;
    }
    if (!has_level) {
        switch (read_num(root, "level_pct", 0, GW_PCT_MAX, &value)) {
            case FIELD_OK:
                cmd->args.set_level.level = level_from_pct((uint8_t)value);
                has_level = true;
                break;
            case FIELD_BAD:
                return cmd_fail(err_msg, err_len, "level_pct must be 0..%d", GW_PCT_MAX);
            case FIELD_ABSENT:
                break;
        }
    }
    if (has_level) {
        switch (read_num(root, "fade_time", 0, 15, &value)) {
            case FIELD_OK:
                cmd->args.set_level.has_fade_time = true;
                cmd->args.set_level.fade_time = (uint8_t)value;
                break;
            case FIELD_BAD:
                return cmd_fail(err_msg, err_len, "fade_time must be 0..15");
            case FIELD_ABSENT:
                break;
        }
        cmd->kind = GW_CMD_SET_LEVEL;
        cmd->urgent = true;
        return ESP_OK;
    }

    bool on = false;
    switch (read_bool(root, "on", &on)) {
        case FIELD_OK:
            cmd->kind = on ? GW_CMD_ON : GW_CMD_OFF;
            cmd->urgent = true;
            return ESP_OK;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "on must be a boolean");
        case FIELD_ABSENT:
            break;
    }

    switch (read_num(root, "scene", 0, GW_MAX_SCENES - 1, &value)) {
        case FIELD_OK:
            cmd->kind = GW_CMD_INDIRECT;
            strlcpy(cmd->args.indirect.name, "go_to_scene", sizeof(cmd->args.indirect.name));
            cmd->args.indirect.scene = (uint8_t)value;
            return ESP_OK;
        case FIELD_BAD:
            return cmd_fail(err_msg, err_len, "scene must be 0..%d", GW_MAX_SCENES - 1);
        case FIELD_ABSENT:
            break;
    }

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (name != NULL && !cJSON_IsNull(name)) {
        if (!cJSON_IsString(name) || name->valuestring == NULL) {
            return cmd_fail(err_msg, err_len, "cmd must be a string");
        }
        return parse_indirect(name->valuestring, cmd, err_msg, err_len);
    }

    bool handled = false;
    CMD_TRY(parse_color(root, cmd, &handled, err_msg, err_len));
    if (!handled) {
        return cmd_fail(err_msg, err_len, "no level, on, scene, cmd or colour in the payload");
    }
    return ESP_OK;
}

esp_err_t gw_api_set_from_payload(const char *payload, size_t len, gw_target_t target,
                                  gw_cmd_t *out, char *err_msg, size_t err_len)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (err_msg != NULL && err_len > 0) {
        err_msg[0] = '\0';
    }
    if (payload == NULL) {
        return cmd_fail(err_msg, err_len, "empty payload");
    }
    /* Publishers routinely append a newline; the bare forms below would not survive it. */
    while (len > 0 && is_ws(payload[0])) {
        payload++;
        len--;
    }
    while (len > 0 && is_ws(payload[len - 1])) {
        len--;
    }
    if (len == 0) {
        return cmd_fail(err_msg, err_len, "empty payload");
    }

    gw_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.target = target;

    /* ON/OFF is what a home-automation system publishes and is not valid JSON, which is the whole
     * reason this function takes bytes rather than a parsed tree. */
    if (token_equals(payload, len, "on") || token_equals(payload, len, "off")) {
        cmd.kind = token_equals(payload, len, "on") ? GW_CMD_ON : GW_CMD_OFF;
        cmd.urgent = true;
        *out = cmd;
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        return cmd_fail(err_msg, err_len, "payload is neither JSON nor ON/OFF");
    }
    esp_err_t err = set_from_json(root, &cmd, err_msg, err_len);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }
    *out = cmd;
    return ESP_OK;
}

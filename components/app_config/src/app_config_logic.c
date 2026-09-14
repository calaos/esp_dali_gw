#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "app_config.h"
#include "app_config_logic.h"

/** Highest usable GPIO, from Kconfig: it differs per chip and this file must stay IDF-free. */
#define GW_GPIO_CEILING CONFIG_GW_GPIO_MAX

/** WPA2 key material: 8..63 ASCII characters or a 64-character hex PSK. */
#define PSK_MIN 8
#define PSK_MAX 64

static void set_err(char *err_field, size_t err_len, const char *path)
{
    if (err_field != NULL && err_len > 0) {
        strlcpy(err_field, path, err_len);
    }
}

#define FAIL(path)                                                                                 \
    do {                                                                                           \
        set_err(err_field, err_len, path);                                                         \
        return ESP_ERR_INVALID_ARG;                                                                \
    } while (0)

/* A candidate may come from a restored blob, so the NUL is not guaranteed to be inside the array
 * and every strlen() below would run off the end without this. */
#define CHECK_TERMINATED(field, path)                                                              \
    do {                                                                                           \
        if (memchr(cfg->field, '\0', sizeof(cfg->field)) == NULL) {                                \
            FAIL(path);                                                                            \
        }                                                                                          \
    } while (0)

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* RFC-1123 single label: the hostname is also the mDNS instance name and the SoftAP suffix. */
static bool hostname_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > APP_CONFIG_NAME_LEN - 1) {
        return false;
    }
    if (s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool alnum = is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum && c != '-') {
            return false;
        }
    }
    return true;
}

static bool ipv4_ok(const char *s)
{
    int octets = 0;
    for (const char *p = s;;) {
        int value = 0;
        int digits = 0;
        while (is_digit(*p)) {
            value = value * 10 + (*p++ - '0');
            if (++digits > 3 || value > 255) {
                return false;
            }
        }
        if (digits == 0 || ++octets > 4) {
            return false;
        }
        if (*p == '\0') {
            return octets == 4;
        }
        if (*p != '.') {
            return false;
        }
        p++;
    }
}

static bool uri_ok(const char *uri)
{
    static const char *const schemes[] = {"mqtt://", "mqtts://", "ws://", "wss://"};
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        size_t n = strlen(schemes[i]);
        if (strncmp(uri, schemes[i], n) == 0) {
            return uri[n] != '\0'; /* a scheme with no authority points nowhere */
        }
    }
    return false;
}

/* Base topics are publish targets, where wildcards and control characters have no meaning. */
static bool topic_ok(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '+' || *p == '#' || *p == ' ' || (unsigned char)*p < 0x20) {
            return false;
        }
    }
    return true;
}

static bool gpio_ok(int8_t gpio)
{
    return gpio >= 0 && gpio <= GW_GPIO_CEILING;
}

static esp_err_t validate_strings(const app_config_t *cfg, char *err_field, size_t err_len)
{
    CHECK_TERMINATED(device.name, "device.name");
    CHECK_TERMINATED(device.hostname, "device.hostname");
    CHECK_TERMINATED(device.timezone, "device.timezone");
    CHECK_TERMINATED(wifi.ssid, "wifi.ssid");
    CHECK_TERMINATED(wifi.password, "wifi.password");
    CHECK_TERMINATED(wifi.ap_password, "wifi.ap_password");
    CHECK_TERMINATED(wifi.country, "wifi.country");
    CHECK_TERMINATED(wifi.static_ip.ip, "wifi.static.ip");
    CHECK_TERMINATED(wifi.static_ip.mask, "wifi.static.mask");
    CHECK_TERMINATED(wifi.static_ip.gw, "wifi.static.gw");
    CHECK_TERMINATED(wifi.static_ip.dns, "wifi.static.dns");
    CHECK_TERMINATED(mqtt.uri, "mqtt.uri");
    CHECK_TERMINATED(mqtt.username, "mqtt.username");
    CHECK_TERMINATED(mqtt.password, "mqtt.password");
    CHECK_TERMINATED(mqtt.client_id, "mqtt.client_id");
    CHECK_TERMINATED(mqtt.base_topic, "mqtt.base_topic");
    CHECK_TERMINATED(mqtt.ha_discovery.prefix, "mqtt.ha_discovery.prefix");
    CHECK_TERMINATED(http.auth.username, "http.auth.username");
    CHECK_TERMINATED(http.auth.password, "http.auth.password");

    for (size_t i = 0; i < APP_CONFIG_MAX_GEARS; i++) {
        if (memchr(cfg->gears[i].name, '\0', sizeof(cfg->gears[i].name)) == NULL) {
            if (err_field != NULL && err_len > 0) {
                snprintf(err_field, err_len, "gears.%u.name", (unsigned)i);
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (size_t i = 0; i < APP_CONFIG_MAX_GROUPS; i++) {
        if (memchr(cfg->groups[i].name, '\0', sizeof(cfg->groups[i].name)) == NULL) {
            if (err_field != NULL && err_len > 0) {
                snprintf(err_field, err_len, "groups.%u.name", (unsigned)i);
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
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
    strlcpy(out->wifi.country, "FR", sizeof(out->wifi.country));
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
    if (err_field != NULL && err_len > 0) {
        err_field[0] = '\0';
    }
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Migration only ever goes forward, so a blob from a newer firmware cannot be read. */
    if (cfg->schema > APP_CONFIG_SCHEMA_VERSION) {
        FAIL("schema");
    }

    esp_err_t err = validate_strings(cfg, err_field, err_len);
    if (err != ESP_OK) {
        return err;
    }

    if (!hostname_ok(cfg->device.hostname)) {
        FAIL("device.hostname");
    }

    /* An empty ssid is legal: that is an unprovisioned device, and an open network has no key. */
    size_t len = strlen(cfg->wifi.password);
    if (len != 0 && (len < PSK_MIN || len > PSK_MAX)) {
        FAIL("wifi.password");
    }
    /* The SoftAP is the recovery path and is always WPA2, so it always needs a usable key. */
    /* Empty is legal and means the world-safe channel set; anything else must be a real domain. */
    len = strlen(cfg->wifi.country);
    if (len != 0) {
        if (len != 2 || cfg->wifi.country[0] < 'A' || cfg->wifi.country[0] > 'Z' ||
            cfg->wifi.country[1] < 'A' || cfg->wifi.country[1] > 'Z') {
            FAIL("wifi.country");
        }
    }

    len = strlen(cfg->wifi.ap_password);
    if (len < PSK_MIN || len > PSK_MAX) {
        FAIL("wifi.ap_password");
    }

    if (cfg->wifi.static_ip.enabled) {
        if (!ipv4_ok(cfg->wifi.static_ip.ip)) {
            FAIL("wifi.static.ip");
        }
        if (!ipv4_ok(cfg->wifi.static_ip.mask)) {
            FAIL("wifi.static.mask");
        }
        if (!ipv4_ok(cfg->wifi.static_ip.gw)) {
            FAIL("wifi.static.gw");
        }
        /* DNS stays optional: a LAN-only install resolves nothing by name. */
        if (cfg->wifi.static_ip.dns[0] != '\0' && !ipv4_ok(cfg->wifi.static_ip.dns)) {
            FAIL("wifi.static.dns");
        }
    }

    /* An empty uri is a broker that has not been configured yet -- which is how a factory device
     * ships, mqtt.enabled and all -- so only a non-empty one has to name a scheme we speak. */
    if (cfg->mqtt.uri[0] != '\0' && !uri_ok(cfg->mqtt.uri)) {
        FAIL("mqtt.uri");
    }
    if (cfg->mqtt.enabled) {
        if (cfg->mqtt.client_id[0] == '\0') {
            FAIL("mqtt.client_id");
        }
        if (!topic_ok(cfg->mqtt.base_topic)) {
            FAIL("mqtt.base_topic");
        }
        if (cfg->mqtt.ha_discovery.enabled && !topic_ok(cfg->mqtt.ha_discovery.prefix)) {
            FAIL("mqtt.ha_discovery.prefix");
        }
    }
    if (cfg->mqtt.qos > 2) {
        FAIL("mqtt.qos");
    }
    /* Below 5 s the client spends its life on PINGREQ; above an hour no broker still cares. */
    if (cfg->mqtt.keepalive_s < 5 || cfg->mqtt.keepalive_s > 3600) {
        FAIL("mqtt.keepalive_s");
    }

    if (cfg->http.auth.enabled) {
        if (cfg->http.auth.username[0] == '\0') {
            FAIL("http.auth.username");
        }
        if (cfg->http.auth.password[0] == '\0') {
            FAIL("http.auth.password");
        }
    }

    if (!gpio_ok(cfg->dali.tx_gpio)) {
        FAIL("dali.tx_gpio");
    }
    if (!gpio_ok(cfg->dali.rx_gpio)) {
        FAIL("dali.rx_gpio");
    }
    if (cfg->dali.tx_gpio == cfg->dali.rx_gpio) {
        FAIL("dali.rx_gpio");
    }
    /* 0 disables polling; anything faster than 5 s would keep the bus permanently busy. */
    if (cfg->dali.poll_interval_s != 0 && cfg->dali.poll_interval_s < 5) {
        FAIL("dali.poll_interval_s");
    }

    if (cfg->led.enabled) {
        if (!gpio_ok(cfg->led.gpio)) {
            FAIL("led.gpio");
        }
        if (cfg->led.gpio == cfg->dali.tx_gpio || cfg->led.gpio == cfg->dali.rx_gpio) {
            FAIL("led.gpio");
        }
    }

    return ESP_OK;
}

void app_config_mask_secrets(app_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    strlcpy(cfg->wifi.password, APP_CONFIG_SECRET_MASK, sizeof(cfg->wifi.password));
    strlcpy(cfg->wifi.ap_password, APP_CONFIG_SECRET_MASK, sizeof(cfg->wifi.ap_password));
    strlcpy(cfg->mqtt.password, APP_CONFIG_SECRET_MASK, sizeof(cfg->mqtt.password));
    strlcpy(cfg->http.auth.password, APP_CONFIG_SECRET_MASK, sizeof(cfg->http.auth.password));
}

static void unmask(char *dst, size_t cap, const char *stored)
{
    if (strcmp(dst, APP_CONFIG_SECRET_MASK) == 0) {
        strlcpy(dst, stored, cap);
    }
}

void app_config_merge_secrets_from(app_config_t *cfg, const app_config_t *stored)
{
    if (cfg == NULL || stored == NULL) {
        return;
    }
    unmask(cfg->wifi.password, sizeof(cfg->wifi.password), stored->wifi.password);
    unmask(cfg->wifi.ap_password, sizeof(cfg->wifi.ap_password), stored->wifi.ap_password);
    unmask(cfg->mqtt.password, sizeof(cfg->mqtt.password), stored->mqtt.password);
    unmask(cfg->http.auth.password, sizeof(cfg->http.auth.password), stored->http.auth.password);
}

/* Field by field rather than memcmp: struct padding is not copied deterministically, and a
 * spurious difference would ask the user to reboot on every save. */
static bool wifi_changed(const app_config_wifi_t *a, const app_config_wifi_t *b)
{
    return strcmp(a->ssid, b->ssid) != 0 || strcmp(a->password, b->password) != 0 ||
           strcmp(a->ap_password, b->ap_password) != 0 || strcmp(a->country, b->country) != 0 ||
           a->fallback_ap_timeout_s != b->fallback_ap_timeout_s ||
           a->static_ip.enabled != b->static_ip.enabled ||
           strcmp(a->static_ip.ip, b->static_ip.ip) != 0 ||
           strcmp(a->static_ip.mask, b->static_ip.mask) != 0 ||
           strcmp(a->static_ip.gw, b->static_ip.gw) != 0 ||
           strcmp(a->static_ip.dns, b->static_ip.dns) != 0;
}

static bool mqtt_changed(const app_config_mqtt_t *a, const app_config_mqtt_t *b)
{
    return a->enabled != b->enabled || strcmp(a->uri, b->uri) != 0 ||
           strcmp(a->username, b->username) != 0 || strcmp(a->password, b->password) != 0 ||
           strcmp(a->client_id, b->client_id) != 0 || strcmp(a->base_topic, b->base_topic) != 0 ||
           a->keepalive_s != b->keepalive_s || a->qos != b->qos ||
           a->retain_state != b->retain_state ||
           a->ha_discovery.enabled != b->ha_discovery.enabled ||
           strcmp(a->ha_discovery.prefix, b->ha_discovery.prefix) != 0;
}

static bool auth_changed(const app_config_http_auth_t *a, const app_config_http_auth_t *b)
{
    return a->enabled != b->enabled || strcmp(a->username, b->username) != 0 ||
           strcmp(a->password, b->password) != 0;
}

app_config_impact_t app_config_diff_impact(const app_config_t *cur, const app_config_t *next)
{
    if (cur == NULL || next == NULL) {
        return APP_CONFIG_IMPACT_NONE;
    }
    unsigned impact = APP_CONFIG_IMPACT_NONE;

    if (wifi_changed(&cur->wifi, &next->wifi) || auth_changed(&cur->http.auth, &next->http.auth) ||
        cur->dali.tx_gpio != next->dali.tx_gpio || cur->dali.rx_gpio != next->dali.rx_gpio ||
        cur->led.gpio != next->led.gpio) {
        impact |= APP_CONFIG_IMPACT_REBOOT;
    }
    if (mqtt_changed(&cur->mqtt, &next->mqtt)) {
        impact |= APP_CONFIG_IMPACT_MQTT_RESTART;
    }
    return (app_config_impact_t)impact;
}

esp_err_t app_config_migrate(app_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->schema == APP_CONFIG_SCHEMA_VERSION) {
        return ESP_OK;
    }
    /* Schema 1 is the first shipped layout, so there is nothing older to upgrade from; a newer
     * one belongs to a firmware this build knows nothing about. */
    return ESP_ERR_INVALID_VERSION;
}

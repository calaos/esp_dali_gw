/**
 * @file app_config.h
 * @brief Typed configuration, NVS persistence, validation and defaults (SPEC 6).
 *
 * The whole configuration is one plain C struct. It is persisted as a single versioned NVS blob
 * (namespace "dali_gw", key "cfg"). JSON lives strictly at the edges: see gw_api.h for the
 * (de)serialization of this struct. Nothing here parses or emits JSON.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bumped when app_config_t changes shape; drives migration on load. */
#define APP_CONFIG_SCHEMA_VERSION 1

#define APP_CONFIG_MAX_GEARS 64  /**< DALI short addresses 0..63. */
#define APP_CONFIG_MAX_GROUPS 16 /**< DALI groups 0..15. */

#define APP_CONFIG_NAME_LEN 33     /**< Friendly names, hostname, usernames, topics prefix. */
#define APP_CONFIG_SSID_LEN 33     /**< 32 chars + NUL, per IEEE 802.11. */
#define APP_CONFIG_PASSWORD_LEN 65 /**< 64 chars + NUL, per WPA2 PSK. */
#define APP_CONFIG_IPADDR_LEN 16   /**< "255.255.255.255" + NUL. */
#define APP_CONFIG_URI_LEN 129     /**< MQTT broker URI. */
#define APP_CONFIG_TOPIC_LEN 65    /**< MQTT base topic. */

/** Placeholder returned instead of any secret, and accepted on write to mean "unchanged". */
#define APP_CONFIG_SECRET_MASK "***"

typedef struct {
    char name[APP_CONFIG_NAME_LEN];
    char hostname[APP_CONFIG_NAME_LEN];
    char timezone[APP_CONFIG_NAME_LEN];
} app_config_device_t;

typedef struct {
    bool enabled;
    char ip[APP_CONFIG_IPADDR_LEN];
    char mask[APP_CONFIG_IPADDR_LEN];
    char gw[APP_CONFIG_IPADDR_LEN];
    char dns[APP_CONFIG_IPADDR_LEN];
} app_config_static_ip_t;

typedef struct {
    char ssid[APP_CONFIG_SSID_LEN];
    char password[APP_CONFIG_PASSWORD_LEN];
    app_config_static_ip_t static_ip;
    char ap_password[APP_CONFIG_PASSWORD_LEN];
    /** ISO 3166-1 alpha-2 regulatory domain. Empty means the world-safe channels 1-11. */
    char country[3];
    /** Seconds spent in STA_CONNECTING before falling back to AP+STA (SPEC 5.1). */
    uint16_t fallback_ap_timeout_s;
} app_config_wifi_t;

typedef struct {
    bool enabled;
    char prefix[APP_CONFIG_NAME_LEN];
} app_config_ha_discovery_t;

typedef struct {
    bool enabled;
    char uri[APP_CONFIG_URI_LEN];
    char username[APP_CONFIG_NAME_LEN];
    char password[APP_CONFIG_PASSWORD_LEN];
    char client_id[APP_CONFIG_NAME_LEN];
    char base_topic[APP_CONFIG_TOPIC_LEN];
    uint16_t keepalive_s;
    uint8_t qos;
    bool retain_state;
    app_config_ha_discovery_t ha_discovery;
} app_config_mqtt_t;

typedef struct {
    bool enabled;
    char username[APP_CONFIG_NAME_LEN];
    char password[APP_CONFIG_PASSWORD_LEN];
} app_config_http_auth_t;

typedef struct {
    app_config_http_auth_t auth;
} app_config_http_t;

typedef struct {
    int8_t tx_gpio;
    int8_t rx_gpio;
    bool invert_tx;
    bool invert_rx;
    /** Seconds between automatic poll_all runs; 0 disables polling. */
    uint16_t poll_interval_s;
    bool scan_on_boot;
    uint16_t identify_blink_ms;
} app_config_dali_t;

typedef struct {
    bool enabled;
    int8_t gpio;
    uint8_t brightness;
} app_config_led_t;

/** Persisted friendly name for one short address or one group. Empty name = never named. */
typedef struct {
    char name[APP_CONFIG_NAME_LEN];
} app_config_label_t;

typedef struct {
    uint16_t schema;
    app_config_device_t device;
    app_config_wifi_t wifi;
    app_config_mqtt_t mqtt;
    app_config_http_t http;
    app_config_dali_t dali;
    app_config_led_t led;
    app_config_label_t gears[APP_CONFIG_MAX_GEARS];
    app_config_label_t groups[APP_CONFIG_MAX_GROUPS];
} app_config_t;

/** Which subsystem a changed field belongs to, so callers know what to restart (SPEC 6). */
typedef enum {
    APP_CONFIG_IMPACT_NONE = 0,
    APP_CONFIG_IMPACT_MQTT_RESTART = 1u << 0, /**< mqtt.* : restart the client in place. */
    APP_CONFIG_IMPACT_REBOOT = 1u << 1,       /**< wifi.*, dali.*gpio*, http.auth, led.gpio. */
} app_config_impact_t;

/**
 * @brief Initialise the store and load the configuration from NVS.
 *
 * Applies defaults for a blank device and migrates older schema versions. Never fails because of a
 * corrupt blob: it falls back to defaults and reports it, so a bad write cannot brick provisioning.
 */
esp_err_t app_config_init(void);

/** @brief Read-only pointer to the live configuration. Never NULL after app_config_init(). */
const app_config_t *app_config_get(void);

/** @brief Fill @p out with the compiled-in defaults, including the MAC-derived identifiers. */
void app_config_defaults(app_config_t *out);

/**
 * @brief Validate a candidate configuration.
 *
 * @param cfg        candidate
 * @param err_field  receives a dotted field path ("mqtt.uri") on failure; may be NULL
 * @param err_len    size of @p err_field
 */
esp_err_t app_config_validate(const app_config_t *cfg, char *err_field, size_t err_len);

/**
 * @brief Validate, persist and adopt @p cfg.
 *
 * Fields left at APP_CONFIG_SECRET_MASK keep their stored value; resolve them with
 * app_config_merge_secrets() before calling if the candidate came from an API client.
 *
 * @param[out] impact  which subsystems the change affects; may be NULL
 */
esp_err_t app_config_set(const app_config_t *cfg, app_config_impact_t *impact);

/** @brief Replace every APP_CONFIG_SECRET_MASK field of @p cfg with the currently stored secret. */
void app_config_merge_secrets(app_config_t *cfg);

/** @brief Overwrite every secret of @p cfg with APP_CONFIG_SECRET_MASK, for serialization. */
void app_config_mask_secrets(app_config_t *cfg);

/** @brief Erase the NVS namespace. The caller reboots; nothing is re-persisted afterwards. */
esp_err_t app_config_factory_reset(void);

/** @brief Device id: last 3 bytes of the base MAC as lowercase hex ("a1b2c3"), 7 bytes needed. */
void app_config_device_id(char *out, size_t len);

/** @brief Persisted name for a short address, or NULL when unnamed. */
const char *app_config_gear_name(uint8_t addr);

/** @brief Persisted name for a group, or NULL when unnamed. */
const char *app_config_group_name(uint8_t group);

/** @brief Persist a friendly name. Passing NULL or "" clears it. */
esp_err_t app_config_set_gear_name(uint8_t addr, const char *name);

/** @brief Persist a friendly group name. Passing NULL or "" clears it. */
esp_err_t app_config_set_group_name(uint8_t group, const char *name);

#ifdef __cplusplus
}
#endif

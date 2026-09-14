#include <string.h>
#include "unity.h"
#include "gw_api.h"

/* Zero-fills first so the bytes past the NUL stay deterministic: the round-trip test compares
 * whole structs, and a leftover tail from a longer default would look like a lost field. */
static void set_str(char *dst, size_t cap, const char *value)
{
    memset(dst, 0, cap);
    memcpy(dst, value, strlen(value));
}

#define SET(field, value) set_str((field), sizeof(field), (value))

static void sample_config(app_config_t *cfg)
{
    app_config_defaults(cfg);
    SET(cfg->device.name, "Hall");
    SET(cfg->device.hostname, "dali-hall");
    SET(cfg->wifi.ssid, "home");
    SET(cfg->wifi.password, "wifi-secret");
    SET(cfg->wifi.ap_password, "ap-secret");
    cfg->wifi.static_ip.enabled = true;
    SET(cfg->wifi.static_ip.ip, "192.168.1.42");
    SET(cfg->wifi.static_ip.mask, "255.255.255.0");
    SET(cfg->wifi.static_ip.gw, "192.168.1.1");
    SET(cfg->wifi.static_ip.dns, "192.168.1.1");
    SET(cfg->mqtt.uri, "mqtts://broker.lan:8883");
    SET(cfg->mqtt.username, "gw");
    SET(cfg->mqtt.password, "mqtt-secret");
    cfg->mqtt.qos = 1;
    cfg->mqtt.ha_discovery.enabled = true;
    cfg->http.auth.enabled = true;
    SET(cfg->http.auth.password, "http-secret");
    cfg->dali.invert_rx = true;
    cfg->led.brightness = 200;
    SET(cfg->gears[3].name, "Kitchen ceiling");
    SET(cfg->gears[63].name, "Cellar");
    SET(cfg->groups[0].name, "Living room");
}

/* Secrets are compared separately, and strlcpy leaves the tail of a masked buffer untouched. */
static void strip_secrets(app_config_t *cfg)
{
    memset(cfg->wifi.password, 0, sizeof(cfg->wifi.password));
    memset(cfg->wifi.ap_password, 0, sizeof(cfg->wifi.ap_password));
    memset(cfg->mqtt.password, 0, sizeof(cfg->mqtt.password));
    memset(cfg->http.auth.password, 0, sizeof(cfg->http.auth.password));
}

static void test_round_trip_preserves_every_field(void)
{
    app_config_t original;
    sample_config(&original);

    cJSON *doc = gw_api_config_to_json(&original, true);
    TEST_ASSERT_NOT_NULL(doc);

    app_config_t restored;
    memset(&restored, 0, sizeof(restored));
    char field[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, gw_api_config_from_json(doc, &restored, field, sizeof(field)));
    TEST_ASSERT_EQUAL_STRING("", field);
    cJSON_Delete(doc);

    TEST_ASSERT_EQUAL_STRING(original.wifi.password, restored.wifi.password);
    TEST_ASSERT_EQUAL_STRING(original.mqtt.password, restored.mqtt.password);

    strip_secrets(&original);
    strip_secrets(&restored);
    TEST_ASSERT_EQUAL_MEMORY(&original, &restored, sizeof(original));
}

static void test_secrets_are_masked_unless_exported(void)
{
    app_config_t cfg;
    sample_config(&cfg);

    cJSON *doc = gw_api_config_to_json(&cfg, false);
    TEST_ASSERT_NOT_NULL(doc);
    const cJSON *wifi = cJSON_GetObjectItem(doc, "wifi");
    const cJSON *mqtt = cJSON_GetObjectItem(doc, "mqtt");
    const cJSON *auth = cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "http"), "auth");
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK,
                             cJSON_GetStringValue(cJSON_GetObjectItem(wifi, "password")));
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK,
                             cJSON_GetStringValue(cJSON_GetObjectItem(wifi, "ap_password")));
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK,
                             cJSON_GetStringValue(cJSON_GetObjectItem(mqtt, "password")));
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK,
                             cJSON_GetStringValue(cJSON_GetObjectItem(auth, "password")));
    /* The ssid is not a secret: masking it would hide which network the device is joining. */
    TEST_ASSERT_EQUAL_STRING("home", cJSON_GetStringValue(cJSON_GetObjectItem(wifi, "ssid")));
    cJSON_Delete(doc);

    doc = gw_api_config_to_json(&cfg, true);
    TEST_ASSERT_NOT_NULL(doc);
    mqtt = cJSON_GetObjectItem(doc, "mqtt");
    TEST_ASSERT_EQUAL_STRING("mqtt-secret",
                             cJSON_GetStringValue(cJSON_GetObjectItem(mqtt, "password")));
    cJSON_Delete(doc);
}

/* Reading a config back and posting it unchanged must not overwrite the stored secrets. */
static void test_masked_secret_keeps_the_stored_value(void)
{
    app_config_t cfg;
    sample_config(&cfg);

    cJSON *doc = gw_api_config_to_json(&cfg, false);
    TEST_ASSERT_NOT_NULL(doc);
    TEST_ASSERT_EQUAL(ESP_OK, gw_api_config_from_json(doc, &cfg, NULL, 0));
    cJSON_Delete(doc);

    TEST_ASSERT_EQUAL_STRING("wifi-secret", cfg.wifi.password);
    TEST_ASSERT_EQUAL_STRING("ap-secret", cfg.wifi.ap_password);
    TEST_ASSERT_EQUAL_STRING("mqtt-secret", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING("http-secret", cfg.http.auth.password);

    cJSON *patch = cJSON_Parse("{\"mqtt\":{\"password\":\"typed-in\"}}");
    TEST_ASSERT_NOT_NULL(patch);
    TEST_ASSERT_EQUAL(ESP_OK, gw_api_config_from_json(patch, &cfg, NULL, 0));
    cJSON_Delete(patch);
    TEST_ASSERT_EQUAL_STRING("typed-in", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING("wifi-secret", cfg.wifi.password);
}

static void test_absent_members_keep_their_value(void)
{
    app_config_t cfg;
    sample_config(&cfg);

    cJSON *patch = cJSON_Parse("{\"led\":{\"brightness\":10}}");
    TEST_ASSERT_NOT_NULL(patch);
    TEST_ASSERT_EQUAL(ESP_OK, gw_api_config_from_json(patch, &cfg, NULL, 0));
    cJSON_Delete(patch);

    TEST_ASSERT_EQUAL(10, cfg.led.brightness);
    TEST_ASSERT_TRUE(cfg.led.enabled);
    TEST_ASSERT_EQUAL_STRING("dali-hall", cfg.device.hostname);
    TEST_ASSERT_EQUAL_STRING("Kitchen ceiling", cfg.gears[3].name);
}

/* Only named entries, keyed by decimal address: 64 empty slots on every read would be absurd. */
static void test_labels_are_a_sparse_object(void)
{
    app_config_t cfg;
    sample_config(&cfg);

    cJSON *doc = gw_api_config_to_json(&cfg, false);
    TEST_ASSERT_NOT_NULL(doc);
    cJSON *gears = cJSON_GetObjectItem(doc, "gears");
    TEST_ASSERT_TRUE(cJSON_IsObject(gears));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(gears));
    TEST_ASSERT_EQUAL_STRING("Kitchen ceiling", cJSON_GetStringValue(cJSON_GetObjectItem(
                                                    cJSON_GetObjectItem(gears, "3"), "name")));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(cJSON_GetObjectItem(doc, "groups")));
    cJSON_Delete(doc);

    /* Clearing a name is an explicit empty string; an absent key means "leave it alone". */
    cJSON *patch = cJSON_Parse("{\"gears\":{\"3\":{\"name\":\"\"}}}");
    TEST_ASSERT_NOT_NULL(patch);
    TEST_ASSERT_EQUAL(ESP_OK, gw_api_config_from_json(patch, &cfg, NULL, 0));
    cJSON_Delete(patch);
    TEST_ASSERT_EQUAL_STRING("", cfg.gears[3].name);
    TEST_ASSERT_EQUAL_STRING("Cellar", cfg.gears[63].name);
}

static void assert_structural_error(const char *json, const char *expected_field)
{
    app_config_t cfg;
    sample_config(&cfg);
    cJSON *doc = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, json);

    char field[64] = {0};
    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG,
                              gw_api_config_from_json(doc, &cfg, field, sizeof(field)), json);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected_field, field, json);
    cJSON_Delete(doc);
}

static void test_structurally_wrong_input_is_rejected(void)
{
    assert_structural_error("{\"mqtt\":{\"qos\":\"one\"}}", "mqtt.qos");
    assert_structural_error("{\"mqtt\":{\"enabled\":1}}", "mqtt.enabled");
    assert_structural_error("{\"device\":{\"name\":42}}", "device.name");
    assert_structural_error("{\"device\":[]}", "device");
    assert_structural_error("{\"wifi\":{\"static\":true}}", "wifi.static");
    assert_structural_error("{\"gears\":{\"64\":{\"name\":\"x\"}}}", "gears.64");
    assert_structural_error("{\"gears\":{\"kitchen\":{\"name\":\"x\"}}}", "gears.kitchen");
    assert_structural_error("{\"gears\":{\"3\":\"Kitchen\"}}", "gears.3");
    assert_structural_error("{\"groups\":{\"16\":{\"name\":\"x\"}}}", "groups.16");
    /* Out of range for the target C type: the cast into the struct would be undefined. */
    assert_structural_error("{\"mqtt\":{\"keepalive_s\":70000}}", "mqtt.keepalive_s");
    assert_structural_error("{\"dali\":{\"tx_gpio\":1000}}", "dali.tx_gpio");
    /* Silently truncating would store something the client never asked for. */
    assert_structural_error("{\"device\":{\"hostname\":\"0123456789012345678901234567890123\"}}",
                            "device.hostname");
}

/*
 * A never-set password must not come back masked: the UI would then offer to "keep the stored
 * value" for a credential that does not exist, and there would be no way to tell the two apart.
 */
static void test_unset_secret_is_empty_not_masked(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);
    TEST_ASSERT_EQUAL_STRING("", cfg.wifi.password);
    strcpy(cfg.mqtt.password, "brokerpass");

    cJSON *doc = gw_api_config_to_json(&cfg, false);
    TEST_ASSERT_NOT_NULL(doc);

    const cJSON *wifi = cJSON_GetObjectItem(doc, "wifi");
    const cJSON *mqtt = cJSON_GetObjectItem(doc, "mqtt");
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(cJSON_GetObjectItem(wifi, "password")));
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK,
                             cJSON_GetStringValue(cJSON_GetObjectItem(mqtt, "password")));

    cJSON_Delete(doc);
}

/* The web UI sends one nested leaf; a shallow merge would drop the sibling secrets with it. */
static void test_partial_patch_keeps_siblings(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);
    strcpy(cfg.wifi.ssid, "HomeNet");
    strcpy(cfg.wifi.password, "supersecret");
    strcpy(cfg.mqtt.password, "brokerpass");

    cJSON *patch = cJSON_Parse("{\"wifi\":{\"ssid\":\"OtherNet\"}}");
    TEST_ASSERT_NOT_NULL(patch);
    char field[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, gw_api_config_from_json(patch, &cfg, field, sizeof(field)));
    cJSON_Delete(patch);

    TEST_ASSERT_EQUAL_STRING("OtherNet", cfg.wifi.ssid);
    TEST_ASSERT_EQUAL_STRING("supersecret", cfg.wifi.password);
    TEST_ASSERT_EQUAL_STRING("brokerpass", cfg.mqtt.password);
    TEST_ASSERT_EQUAL(60, cfg.wifi.fallback_ap_timeout_s);
}

void test_gw_api_config_run(void)
{
    RUN_TEST(test_unset_secret_is_empty_not_masked);
    RUN_TEST(test_partial_patch_keeps_siblings);
    RUN_TEST(test_round_trip_preserves_every_field);
    RUN_TEST(test_secrets_are_masked_unless_exported);
    RUN_TEST(test_masked_secret_keeps_the_stored_value);
    RUN_TEST(test_absent_members_keep_their_value);
    RUN_TEST(test_labels_are_a_sparse_object);
    RUN_TEST(test_structurally_wrong_input_is_rejected);
}

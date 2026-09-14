#include <string.h>
#include "unity.h"
#include "app_config.h"
#include "app_config_logic.h"

/* Every case starts from the shipped defaults, so a failure means the rule under test broke and
 * not that a hand-built fixture drifted. */

static void assert_rejects(app_config_t *cfg, const char *expected_field)
{
    char field[APP_CONFIG_NAME_LEN] = {0};
    TEST_ASSERT_NOT_EQUAL(ESP_OK, app_config_validate(cfg, field, sizeof(field)));
    TEST_ASSERT_EQUAL_STRING(expected_field, field);
}

static void assert_accepts(app_config_t *cfg)
{
    char field[APP_CONFIG_NAME_LEN] = {0};
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, app_config_validate(cfg, field, sizeof(field)), field);
    TEST_ASSERT_EQUAL_STRING("", field);
}

static void test_defaults_are_valid(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);
    assert_accepts(&cfg);
    TEST_ASSERT_EQUAL(APP_CONFIG_SCHEMA_VERSION, cfg.schema);
    /* A factory device is unprovisioned; that is what sends it to AP mode (SPEC 5.1). */
    TEST_ASSERT_EQUAL_STRING("", cfg.wifi.ssid);
}

static void test_hostname_follows_rfc1123(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);

    strcpy(cfg.device.hostname, "dali-gw-1");
    assert_accepts(&cfg);

    strcpy(cfg.device.hostname, "-dali");
    assert_rejects(&cfg, "device.hostname");
    strcpy(cfg.device.hostname, "dali-");
    assert_rejects(&cfg, "device.hostname");
    strcpy(cfg.device.hostname, "dali gw");
    assert_rejects(&cfg, "device.hostname");
    strcpy(cfg.device.hostname, "dali_gw");
    assert_rejects(&cfg, "device.hostname");
    strcpy(cfg.device.hostname, "");
    assert_rejects(&cfg, "device.hostname");
}

static void test_mqtt_uri_scheme(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);

    const char *good[] = {"mqtt://10.0.0.1:1883", "mqtts://broker.lan:8883", "ws://h/mqtt",
                          "wss://h/mqtt"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        strcpy(cfg.mqtt.uri, good[i]);
        assert_accepts(&cfg);
    }

    const char *bad[] = {"http://broker", "tcp://broker", "broker:1883", "mqtt:/broker", "mqtt://"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        strcpy(cfg.mqtt.uri, bad[i]);
        assert_rejects(&cfg, "mqtt.uri");
    }

    /* A factory device ships mqtt.enabled with no broker yet; that is not an error. */
    strcpy(cfg.mqtt.uri, "");
    assert_accepts(&cfg);
}

static void test_wifi_key_lengths(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);

    strcpy(cfg.wifi.ssid, "home");
    strcpy(cfg.wifi.password, ""); /* open network */
    assert_accepts(&cfg);
    strcpy(cfg.wifi.password, "1234567");
    assert_rejects(&cfg, "wifi.password");
    strcpy(cfg.wifi.password, "12345678");
    assert_accepts(&cfg);

    strcpy(cfg.wifi.ap_password, "short");
    assert_rejects(&cfg, "wifi.ap_password");
    strcpy(cfg.wifi.ap_password, "");
    assert_rejects(&cfg, "wifi.ap_password");
}

static void test_static_ip_quads(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);

    cfg.wifi.static_ip.enabled = true;
    assert_rejects(&cfg, "wifi.static.ip");

    strcpy(cfg.wifi.static_ip.ip, "192.168.1.42");
    strcpy(cfg.wifi.static_ip.mask, "255.255.255.0");
    strcpy(cfg.wifi.static_ip.gw, "192.168.1.1");
    assert_accepts(&cfg); /* DNS stays optional */

    strcpy(cfg.wifi.static_ip.dns, "192.168.1");
    assert_rejects(&cfg, "wifi.static.dns");
    strcpy(cfg.wifi.static_ip.dns, "1.1.1.256");
    assert_rejects(&cfg, "wifi.static.dns");
    strcpy(cfg.wifi.static_ip.dns, "1.1.1.1");
    assert_accepts(&cfg);
}

static void test_ranges_and_gpios(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);

    cfg.mqtt.qos = 3;
    assert_rejects(&cfg, "mqtt.qos");
    cfg.mqtt.qos = 2;
    assert_accepts(&cfg);

    cfg.mqtt.keepalive_s = 0;
    assert_rejects(&cfg, "mqtt.keepalive_s");
    cfg.mqtt.keepalive_s = 30;

    cfg.dali.poll_interval_s = 1;
    assert_rejects(&cfg, "dali.poll_interval_s");
    cfg.dali.poll_interval_s = 0; /* polling off */
    assert_accepts(&cfg);

    cfg.dali.tx_gpio = 99;
    assert_rejects(&cfg, "dali.tx_gpio");
    cfg.dali.tx_gpio = cfg.dali.rx_gpio;
    assert_rejects(&cfg, "dali.rx_gpio");
    cfg.dali.tx_gpio = 14;

    cfg.led.gpio = cfg.dali.rx_gpio;
    assert_rejects(&cfg, "led.gpio");
    cfg.led.enabled = false; /* an unused LED pin is nobody's business */
    assert_accepts(&cfg);
}

static void test_http_auth_needs_credentials(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);

    cfg.http.auth.enabled = true;
    assert_rejects(&cfg, "http.auth.password");
    strcpy(cfg.http.auth.password, "hunter2");
    assert_accepts(&cfg);
    strcpy(cfg.http.auth.username, "");
    assert_rejects(&cfg, "http.auth.username");
}

/* A restored blob is not guaranteed to hold a NUL, and every strlen() downstream depends on it. */
static void test_unterminated_string_is_rejected(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);
    memset(cfg.device.name, 'x', sizeof(cfg.device.name));
    assert_rejects(&cfg, "device.name");

    app_config_defaults(&cfg);
    memset(cfg.gears[7].name, 'x', sizeof(cfg.gears[7].name));
    assert_rejects(&cfg, "gears.7.name");
}

static void test_schema_from_the_future_is_rejected(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);
    cfg.schema = APP_CONFIG_SCHEMA_VERSION + 1;
    assert_rejects(&cfg, "schema");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, app_config_migrate(&cfg));

    cfg.schema = APP_CONFIG_SCHEMA_VERSION;
    TEST_ASSERT_EQUAL(ESP_OK, app_config_migrate(&cfg));
}

static void test_mask_replaces_every_secret(void)
{
    app_config_t cfg;
    app_config_defaults(&cfg);
    strcpy(cfg.wifi.password, "wifi-secret");
    strcpy(cfg.wifi.ap_password, "ap-secret");
    strcpy(cfg.mqtt.password, "mqtt-secret");
    strcpy(cfg.http.auth.password, "http-secret");

    app_config_mask_secrets(&cfg);
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK, cfg.wifi.password);
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK, cfg.wifi.ap_password);
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK, cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING(APP_CONFIG_SECRET_MASK, cfg.http.auth.password);
    /* Non-secrets must survive masking: the masked copy is what the UI renders. */
    TEST_ASSERT_EQUAL_STRING("admin", cfg.http.auth.username);
}

static void test_merge_restores_only_masked_secrets(void)
{
    app_config_t stored;
    app_config_defaults(&stored);
    strcpy(stored.wifi.password, "wifi-secret");
    strcpy(stored.wifi.ap_password, "ap-secret");
    strcpy(stored.mqtt.password, "mqtt-secret");
    strcpy(stored.http.auth.password, "http-secret");

    app_config_t candidate = stored;
    app_config_mask_secrets(&candidate);
    strcpy(candidate.mqtt.password, "brand-new"); /* the one the user actually retyped */

    app_config_merge_secrets_from(&candidate, &stored);
    TEST_ASSERT_EQUAL_STRING("wifi-secret", candidate.wifi.password);
    TEST_ASSERT_EQUAL_STRING("ap-secret", candidate.wifi.ap_password);
    TEST_ASSERT_EQUAL_STRING("http-secret", candidate.http.auth.password);
    TEST_ASSERT_EQUAL_STRING("brand-new", candidate.mqtt.password);

    /* The reverse direction must not leak: masking never reveals the stored value. */
    app_config_t masked = stored;
    app_config_mask_secrets(&masked);
    TEST_ASSERT_NULL(strstr(masked.wifi.password, "secret"));
}

static void test_impact_matches_the_spec(void)
{
    app_config_t cur;
    app_config_defaults(&cur);
    app_config_t next = cur;

    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_NONE, app_config_diff_impact(&cur, &next));

    next.device.name[0] = 'X';
    next.dali.poll_interval_s = 60;
    next.led.brightness = 200;
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_NONE, app_config_diff_impact(&cur, &next));

    next = cur;
    strcpy(next.mqtt.uri, "mqtt://other:1883");
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_MQTT_RESTART, app_config_diff_impact(&cur, &next));

    next = cur;
    strcpy(next.wifi.ssid, "other");
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_REBOOT, app_config_diff_impact(&cur, &next));

    next = cur;
    next.http.auth.enabled = true;
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_REBOOT, app_config_diff_impact(&cur, &next));

    next = cur;
    next.dali.tx_gpio = 15;
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_REBOOT, app_config_diff_impact(&cur, &next));

    next = cur;
    next.led.gpio = 10;
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_REBOOT, app_config_diff_impact(&cur, &next));

    next = cur;
    strcpy(next.wifi.ssid, "other");
    next.mqtt.qos = 1;
    TEST_ASSERT_EQUAL(APP_CONFIG_IMPACT_REBOOT | APP_CONFIG_IMPACT_MQTT_RESTART,
                      app_config_diff_impact(&cur, &next));
}

void test_app_config_run(void)
{
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_hostname_follows_rfc1123);
    RUN_TEST(test_mqtt_uri_scheme);
    RUN_TEST(test_wifi_key_lengths);
    RUN_TEST(test_static_ip_quads);
    RUN_TEST(test_ranges_and_gpios);
    RUN_TEST(test_http_auth_needs_credentials);
    RUN_TEST(test_unterminated_string_is_rejected);
    RUN_TEST(test_schema_from_the_future_is_rejected);
    RUN_TEST(test_mask_replaces_every_secret);
    RUN_TEST(test_merge_restores_only_masked_secrets);
    RUN_TEST(test_impact_matches_the_spec);
}

/* The wire schema of SPEC 7.3, 8.1, 8.2 and 8.3. Both adapters serialize through gw_api, so a
 * shape change here is a break for every client at once. */
#include <string.h>
#include "unity.h"
#include "gw_api.h"

static gw_gear_t sample_gear(void)
{
    gw_gear_t gear;
    memset(&gear, 0, sizeof(gear));
    gear.addr = 3;
    strcpy(gear.name, "Kitchen ceiling");
    gear.present = true;
    gear.last_seen = 1726300000;
    gear.device_types[0] = 6;
    gear.device_type_count = 1;
    gear.version_major = 2;
    gear.version_minor = 0;
    gear.level = 128;
    gear.level_valid = true;
    gear.status.valid = true;
    gear.status.raw = 4;
    gear.status.lamp_on = true;
    return gear;
}

static void fill_config(gw_gear_t *gear)
{
    gear->config.valid = true;
    gear->config.min = 85;
    gear->config.max = 254;
    gear->config.power_on = 254;
    gear->config.system_failure = 254;
    gear->config.fade_time = 4;
    gear->config.fade_rate = 7;
    gear->config.physical_min = 85;
    gear->config.groups = (1u << 0) | (1u << 3);
    for (size_t i = 0; i < GW_MAX_SCENES; i++) {
        gear->config.scenes[i] = -1;
    }
    gear->config.scenes[0] = 254;
    gear->config.scenes[1] = 0;
    gear->config.scenes[2] = 128;
}

static cJSON *parse_set(const char *payload, gw_cmd_t *out)
{
    gw_target_t target = {.type = GW_TARGET_SHORT, .addr = 3};
    char err[96] = "";
    TEST_ASSERT_EQUAL_MESSAGE(
        ESP_OK, gw_api_set_from_payload(payload, strlen(payload), target, out, err, sizeof(err)),
        payload);
    return NULL;
}

static void reject_set(const char *payload)
{
    gw_target_t target = {.type = GW_TARGET_SHORT, .addr = 3};
    gw_cmd_t cmd;
    char err[96] = "";
    TEST_ASSERT_EQUAL_MESSAGE(
        ESP_ERR_INVALID_ARG,
        gw_api_set_from_payload(payload, strlen(payload), target, &cmd, err, sizeof(err)), payload);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, err[0], "a rejection must carry a message");
    TEST_ASSERT_EQUAL(GW_CMD_NONE, cmd.kind);
}

static esp_err_t parse_cmd(const char *action, const char *json, gw_cmd_t *out, char *err,
                           size_t err_len)
{
    cJSON *root = json != NULL ? cJSON_Parse(json) : NULL;
    if (json != NULL) {
        TEST_ASSERT_NOT_NULL_MESSAGE(root, json);
    }
    esp_err_t rc = gw_api_cmd_from_json(action, root, out, err, err_len);
    cJSON_Delete(root);
    return rc;
}

/* --- gear object (SPEC 7.3) ------------------------------------------------------------------ */

static void test_gear_object_has_the_spec_shape(void)
{
    gw_gear_t gear = sample_gear();
    cJSON *obj = gw_api_gear_to_json(&gear, false);
    TEST_ASSERT_NOT_NULL(obj);

    TEST_ASSERT_EQUAL(3, cJSON_GetObjectItem(obj, "addr")->valueint);
    TEST_ASSERT_EQUAL_STRING("Kitchen ceiling", cJSON_GetObjectItem(obj, "name")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(obj, "present")));
    TEST_ASSERT_EQUAL(1726300000, (long)cJSON_GetObjectItem(obj, "last_seen")->valuedouble);

    cJSON *types = cJSON_GetObjectItem(obj, "device_types");
    TEST_ASSERT_TRUE(cJSON_IsArray(types));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(types));
    TEST_ASSERT_EQUAL(6, cJSON_GetArrayItem(types, 0)->valueint);

    /* The version is a string, not two numbers: "2.0" is what the UI and HA discovery show. */
    TEST_ASSERT_EQUAL_STRING("2.0", cJSON_GetObjectItem(obj, "version")->valuestring);

    TEST_ASSERT_EQUAL(128, cJSON_GetObjectItem(obj, "level")->valueint);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(obj, "level_pct"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(obj, "on")));

    cJSON *status = cJSON_GetObjectItem(obj, "status");
    TEST_ASSERT_EQUAL(4, cJSON_GetObjectItem(status, "raw")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(status, "lamp_on")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(status, "gear_failure")));
    TEST_ASSERT_EQUAL(9, cJSON_GetArraySize(status));

    /* Shallow: no config, no identity, and no dt8 on a gear that does not report DT8. */
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "config"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "identity"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "dt8"));
    cJSON_Delete(obj);
}

static void test_gear_deep_adds_config_and_identity(void)
{
    gw_gear_t gear = sample_gear();
    fill_config(&gear);
    gear.identity.valid = true;
    strcpy(gear.identity.gtin, "4052899000000");
    strcpy(gear.identity.serial, "0000000123");
    gear.identity.bank0_version = 1;
    gear.dt8.supported = true;
    gear.dt8.caps = 0x30;
    gear.dt8.tc_min_mirek = 153;
    gear.dt8.tc_max_mirek = 370;

    cJSON *obj = gw_api_gear_to_json(&gear, true);
    TEST_ASSERT_NOT_NULL(obj);

    cJSON *dt8 = cJSON_GetObjectItem(obj, "dt8");
    TEST_ASSERT_EQUAL(0x30, cJSON_GetObjectItem(dt8, "caps")->valueint);
    TEST_ASSERT_EQUAL(153, cJSON_GetObjectItem(dt8, "tc_min")->valueint);
    TEST_ASSERT_EQUAL(370, cJSON_GetObjectItem(dt8, "tc_max")->valueint);

    cJSON *cfg = cJSON_GetObjectItem(obj, "config");
    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_EQUAL(85, cJSON_GetObjectItem(cfg, "min")->valueint);
    TEST_ASSERT_EQUAL(85, cJSON_GetObjectItem(cfg, "physical_min")->valueint);

    /* groups is the list of memberships, never the raw membership word. */
    cJSON *groups = cJSON_GetObjectItem(cfg, "groups");
    TEST_ASSERT_TRUE(cJSON_IsArray(groups));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(groups));
    TEST_ASSERT_EQUAL(0, cJSON_GetArrayItem(groups, 0)->valueint);
    TEST_ASSERT_EQUAL(3, cJSON_GetArrayItem(groups, 1)->valueint);

    cJSON *scenes = cJSON_GetObjectItem(cfg, "scenes");
    TEST_ASSERT_EQUAL(GW_MAX_SCENES, cJSON_GetArraySize(scenes));
    TEST_ASSERT_EQUAL(254, cJSON_GetArrayItem(scenes, 0)->valueint);
    TEST_ASSERT_EQUAL(0, cJSON_GetArrayItem(scenes, 1)->valueint);
    TEST_ASSERT_EQUAL(128, cJSON_GetArrayItem(scenes, 2)->valueint);
    /* 0xFF is "not programmed" and must not be mistaken for level 255. */
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetArrayItem(scenes, 3)));

    cJSON *id = cJSON_GetObjectItem(obj, "identity");
    TEST_ASSERT_EQUAL_STRING("4052899000000", cJSON_GetObjectItem(id, "gtin")->valuestring);
    TEST_ASSERT_EQUAL_STRING("0000000123", cJSON_GetObjectItem(id, "serial")->valuestring);
    TEST_ASSERT_EQUAL(1, cJSON_GetObjectItem(id, "bank0_version")->valueint);
    cJSON_Delete(obj);
}

/* "Never read" and "read as zero" have to stay distinguishable for a client. */
static void test_gear_omits_blocks_that_were_never_read(void)
{
    gw_gear_t gear = sample_gear();
    gear.level_valid = false;
    gear.status.valid = false;
    gear.status.raw = 0;

    cJSON *obj = gw_api_gear_to_json(&gear, true);
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "config"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "identity"));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(obj, "level")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(obj, "level_pct")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(obj, "on")));

    cJSON *status = cJSON_GetObjectItem(obj, "status");
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(status, "raw")));
    TEST_ASSERT_EQUAL_MESSAGE(1, cJSON_GetArraySize(status),
                              "eight false bits would read as a healthy gear");
    cJSON_Delete(obj);
}

/* --- level_pct (SPEC 17: linear) -------------------------------------------------------------- */

static int pct_of_level(uint8_t level)
{
    gw_gear_t gear = sample_gear();
    gear.level = level;
    cJSON *obj = gw_api_gear_to_json(&gear, false);
    TEST_ASSERT_NOT_NULL(obj);
    int pct = cJSON_GetObjectItem(obj, "level_pct")->valueint;
    cJSON_Delete(obj);
    return pct;
}

static int level_of_pct(int pct)
{
    char payload[32];
    gw_cmd_t cmd;
    snprintf(payload, sizeof(payload), "{\"level_pct\":%d}", pct);
    parse_set(payload, &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_SET_LEVEL, cmd.kind);
    return cmd.args.set_level.level;
}

static void test_level_pct_is_linear_at_the_edges(void)
{
    TEST_ASSERT_EQUAL(0, pct_of_level(0));
    TEST_ASSERT_EQUAL(1, pct_of_level(1));
    TEST_ASSERT_EQUAL(100, pct_of_level(254));

    TEST_ASSERT_EQUAL(0, level_of_pct(0));
    TEST_ASSERT_EQUAL(1, level_of_pct(1));
    TEST_ASSERT_EQUAL(254, level_of_pct(100));
}

/* Reported level_pct and accepted level_pct come from the same mapping, so they cannot drift. */
static void test_level_pct_round_trips(void)
{
    for (int pct = 0; pct <= 100; pct++) {
        int level = level_of_pct(pct);
        TEST_ASSERT_EQUAL_MESSAGE(pct, pct_of_level((uint8_t)level), "pct -> level -> pct");
    }
}

/* --- compact list (SPEC 8.1) ------------------------------------------------------------------ */

static void test_gears_list_is_compact(void)
{
    gw_gear_t gears[2];
    memset(gears, 0, sizeof(gears));
    gears[0] = sample_gear();
    gears[1].addr = 7;
    strcpy(gears[1].name, "Hall");

    cJSON *root = gw_api_gears_to_json(gears, 2);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(root));

    cJSON *arr = cJSON_GetObjectItem(root, "gears");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(arr));

    cJSON *first = cJSON_GetArrayItem(arr, 0);
    TEST_ASSERT_EQUAL(6, cJSON_GetArraySize(first));
    TEST_ASSERT_EQUAL(3, cJSON_GetObjectItem(first, "addr")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(first, "present")));
    TEST_ASSERT_EQUAL(128, cJSON_GetObjectItem(first, "level")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(first, "on")));
    /* status carries raw only; the decoded bits belong to the full gear object. */
    cJSON *status = cJSON_GetObjectItem(first, "status");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(status));
    TEST_ASSERT_EQUAL(4, cJSON_GetObjectItem(status, "raw")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(first, "level_pct"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(first, "device_types"));

    cJSON *second = cJSON_GetArrayItem(arr, 1);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(second, "level")));
    cJSON_Delete(root);
}

/* --- bus and progress (SPEC 8.1) -------------------------------------------------------------- */

static void test_bus_reports_null_when_idle(void)
{
    gw_bus_status_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.powered = true;
    bus.gear_count = 5;
    bus.last_scan = 1726300000;

    cJSON *obj = gw_api_bus_to_json(&bus);
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_EQUAL(6, cJSON_GetArraySize(obj));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(obj, "powered")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(obj, "busy")));
    /* null, never "": a client tests for the absence of an operation. */
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(obj, "operation")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(obj, "progress")));
    TEST_ASSERT_EQUAL(5, cJSON_GetObjectItem(obj, "gear_count")->valueint);
    cJSON_Delete(obj);
}

static void test_bus_reports_progress_while_scanning(void)
{
    gw_bus_status_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.powered = true;
    bus.busy = true;
    strcpy(bus.operation, "scan");
    bus.has_progress = true;
    bus.done = 17;
    bus.total = 64;
    bus.found = 3;

    cJSON *obj = gw_api_bus_to_json(&bus);
    TEST_ASSERT_EQUAL_STRING("scan", cJSON_GetObjectItem(obj, "operation")->valuestring);
    cJSON *progress = cJSON_GetObjectItem(obj, "progress");
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(progress));
    TEST_ASSERT_EQUAL(17, cJSON_GetObjectItem(progress, "done")->valueint);
    TEST_ASSERT_EQUAL(64, cJSON_GetObjectItem(progress, "total")->valueint);
    TEST_ASSERT_EQUAL(3, cJSON_GetObjectItem(progress, "found")->valueint);
    cJSON_Delete(obj);
}

static void test_progress_event(void)
{
    cJSON *obj = gw_api_progress_to_json("scan", 17, 64, 3);
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_EQUAL(4, cJSON_GetArraySize(obj));
    TEST_ASSERT_EQUAL_STRING("scan", cJSON_GetObjectItem(obj, "operation")->valuestring);
    TEST_ASSERT_EQUAL(17, cJSON_GetObjectItem(obj, "done")->valueint);
    cJSON_Delete(obj);
}

/* --- result envelope (SPEC 8.3) --------------------------------------------------------------- */

static void test_result_ok_carries_data(void)
{
    gw_result_t res;
    memset(&res, 0, sizeof(res));
    res.id = 2;
    strcpy(res.action, "commission");
    res.ok = true;
    res.duration_ms = 8420;
    res.data = cJSON_CreateObject();
    cJSON_AddNumberToObject(res.data, "assigned", 3);

    cJSON *obj = gw_api_result_to_json(&res);
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_EQUAL(2, cJSON_GetObjectItem(obj, "id")->valueint);
    TEST_ASSERT_EQUAL_STRING("commission", cJSON_GetObjectItem(obj, "action")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(obj, "ok")));
    TEST_ASSERT_EQUAL(8420, cJSON_GetObjectItem(obj, "duration_ms")->valueint);
    TEST_ASSERT_EQUAL(3,
                      cJSON_GetObjectItem(cJSON_GetObjectItem(obj, "data"), "assigned")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "error"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "target"));

    /* data is copied, not transferred: the result still owns it and still frees it. */
    gw_api_result_free(&res);
    TEST_ASSERT_EQUAL(3,
                      cJSON_GetObjectItem(cJSON_GetObjectItem(obj, "data"), "assigned")->valueint);
    cJSON_Delete(obj);
}

static void test_result_error_carries_message_and_target(void)
{
    gw_result_t res;
    memset(&res, 0, sizeof(res));
    res.id = 7;
    strcpy(res.action, "set_level");
    res.ok = false;
    res.error = GW_ERR_NO_REPLY;
    strcpy(res.message, "gear 3 did not answer");
    res.has_target = true;
    res.target.type = GW_TARGET_SHORT;
    res.target.addr = 3;

    cJSON *obj = gw_api_result_to_json(&res);
    TEST_ASSERT_EQUAL_STRING("no_reply", cJSON_GetObjectItem(obj, "error")->valuestring);
    TEST_ASSERT_EQUAL_STRING("gear 3 did not answer",
                             cJSON_GetObjectItem(obj, "message")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(obj, "data"));
    cJSON *target = cJSON_GetObjectItem(obj, "target");
    TEST_ASSERT_EQUAL_STRING("short", cJSON_GetObjectItem(target, "type")->valuestring);
    TEST_ASSERT_EQUAL(3, cJSON_GetObjectItem(target, "addr")->valueint);
    cJSON_Delete(obj);
}

/* A broadcast has no address; a 0 here would read as short address 0. */
static void test_result_broadcast_target_has_no_addr(void)
{
    gw_result_t res;
    memset(&res, 0, sizeof(res));
    strcpy(res.action, "off");
    res.ok = true;
    res.has_target = true;
    res.target.type = GW_TARGET_BROADCAST;

    cJSON *obj = gw_api_result_to_json(&res);
    cJSON *target = cJSON_GetObjectItem(obj, "target");
    TEST_ASSERT_EQUAL_STRING("broadcast", cJSON_GetObjectItem(target, "type")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(target, "addr"));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(target));
    cJSON_Delete(obj);
}

/* --- set payloads (SPEC 8.2) ------------------------------------------------------------------ */

static void test_set_accepts_the_bare_forms(void)
{
    gw_cmd_t cmd;

    parse_set("128", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_SET_LEVEL, cmd.kind);
    TEST_ASSERT_EQUAL(128, cmd.args.set_level.level);
    /* SPEC 7.2 and 17: a level must be served before the next step of a running scan. */
    TEST_ASSERT_TRUE(cmd.urgent);
    TEST_ASSERT_EQUAL(GW_TARGET_SHORT, cmd.target.type);
    TEST_ASSERT_EQUAL(3, cmd.target.addr);

    parse_set("ON", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_ON, cmd.kind);
    TEST_ASSERT_TRUE(cmd.urgent);

    parse_set("off", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_OFF, cmd.kind);
    TEST_ASSERT_TRUE(cmd.urgent);

    /* Publishers append newlines; the bare forms have to survive that. */
    parse_set(" On\n", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_ON, cmd.kind);
}

static void test_set_accepts_the_json_forms(void)
{
    gw_cmd_t cmd;

    parse_set("{\"on\":false}", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_OFF, cmd.kind);
    TEST_ASSERT_TRUE(cmd.urgent);

    parse_set("{\"level_pct\":50}", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_SET_LEVEL, cmd.kind);
    TEST_ASSERT_EQUAL(126, cmd.args.set_level.level);
    TEST_ASSERT_FALSE(cmd.args.set_level.has_fade_time);

    parse_set("{\"level_pct\":50,\"fade_time\":4}", &cmd);
    TEST_ASSERT_TRUE(cmd.args.set_level.has_fade_time);
    TEST_ASSERT_EQUAL(4, cmd.args.set_level.fade_time);

    parse_set("{\"scene\":2}", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_INDIRECT, cmd.kind);
    TEST_ASSERT_EQUAL_STRING("go_to_scene", cmd.args.indirect.name);
    TEST_ASSERT_EQUAL(2, cmd.args.indirect.scene);
    TEST_ASSERT_FALSE(cmd.urgent);

    parse_set("{\"cmd\":\"up\"}", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_INDIRECT, cmd.kind);
    TEST_ASSERT_EQUAL_STRING("up", cmd.args.indirect.name);

    parse_set("{\"cmd\":\"go_to_scene:3\"}", &cmd);
    TEST_ASSERT_EQUAL_STRING("go_to_scene", cmd.args.indirect.name);
    TEST_ASSERT_EQUAL(3, cmd.args.indirect.scene);

    parse_set("{\"mirek\":300}", &cmd);
    TEST_ASSERT_EQUAL(GW_CMD_COLOR, cmd.kind);
    TEST_ASSERT_EQUAL(GW_COLOR_MIREK, cmd.args.color.kind);
    TEST_ASSERT_EQUAL(300, cmd.args.color.mirek);

    /* mirek = 1e6 / K, and the two spellings have to land on the same command. */
    parse_set("{\"kelvin\":4000}", &cmd);
    TEST_ASSERT_EQUAL(GW_COLOR_MIREK, cmd.args.color.kind);
    TEST_ASSERT_EQUAL(250, cmd.args.color.mirek);

    parse_set("{\"rgb\":[254,0,0]}", &cmd);
    TEST_ASSERT_EQUAL(GW_COLOR_RGB, cmd.args.color.kind);
    TEST_ASSERT_EQUAL(254, cmd.args.color.channels[0]);
    TEST_ASSERT_EQUAL(0, cmd.args.color.channels[1]);

    parse_set("{\"rgbwaf\":[1,2,3,4,5,6]}", &cmd);
    TEST_ASSERT_EQUAL(GW_COLOR_RGBWAF, cmd.args.color.kind);
    TEST_ASSERT_EQUAL(6, cmd.args.color.channels[5]);
}

static void test_set_rejects_nonsense(void)
{
    reject_set("");
    reject_set("MAYBE");
    reject_set("255");
    reject_set("-1");
    reject_set("{}");
    reject_set("{\"level\":300}");
    reject_set("{\"level_pct\":101}");
    reject_set("{\"kelvin\":0}");
    reject_set("{\"rgb\":[1,2]}");
    reject_set("{\"scene\":16}");
    reject_set("{\"on\":\"yes\"}");
}

/* --- cmd payloads (SPEC 8.2) ------------------------------------------------------------------ */

static void test_cmd_scan_and_cancel(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_OK,
                      parse_cmd("scan", "{\"id\":1,\"deep\":true}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_SCAN, cmd.kind);
    TEST_ASSERT_EQUAL(1, cmd.id);
    TEST_ASSERT_TRUE(cmd.args.scan.deep);

    /* An action with no arguments must work with an empty payload and with none at all. */
    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("scan", "{}", &cmd, err, sizeof(err)));
    TEST_ASSERT_FALSE(cmd.args.scan.deep);
    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("cancel", NULL, &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_CANCEL, cmd.kind);
    TEST_ASSERT_TRUE(cmd.urgent);

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("bus_check", "{}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_BUS_CHECK, cmd.kind);
    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("poll_all", "{}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_POLL_ALL, cmd.kind);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_cmd("nope", "{}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_NONE, cmd.kind);
}

/* Re-addressing the whole bus is destructive, so it needs an explicit confirmation (SPEC 17). */
static void test_commission_all_needs_confirm(void)
{
    gw_cmd_t cmd;
    char err[96] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("commission", "{\"mode\":\"all\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_NOT_EQUAL(0, err[0]);
    TEST_ASSERT_EQUAL(GW_CMD_NONE, cmd.kind);

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        parse_cmd("commission", "{\"mode\":\"all\",\"confirm\":false}", &cmd, err, sizeof(err)));

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("commission", "{\"mode\":\"all\",\"confirm\":true}", &cmd,
                                        err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_COMMISSION_ALL, cmd.args.commission.mode);

    /* "unaddressed" only adds addresses, so it needs no confirmation -- and is the default. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      parse_cmd("commission", "{\"start_addr\":4}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_COMMISSION_UNADDRESSED, cmd.args.commission.mode);
    TEST_ASSERT_EQUAL(4, cmd.args.commission.start_addr);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("commission", "{\"mode\":\"some\"}", &cmd, err, sizeof(err)));
}

static void test_raw_frame_is_4_or_6_hex_digits(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("raw", "{\"frame\":\"FF08\",\"send_twice\":true}", &cmd,
                                        err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_RAW, cmd.kind);
    TEST_ASSERT_EQUAL_HEX32(0xFF08, cmd.args.raw.frame);
    TEST_ASSERT_EQUAL(16, cmd.args.raw.bits);
    TEST_ASSERT_TRUE(cmd.args.raw.send_twice);
    TEST_ASSERT_FALSE(cmd.args.raw.expect_reply);

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("raw", "{\"frame\":\"a1b2c3\",\"expect_reply\":true}", &cmd,
                                        err, sizeof(err)));
    TEST_ASSERT_EQUAL_HEX32(0xA1B2C3, cmd.args.raw.frame);
    TEST_ASSERT_EQUAL(24, cmd.args.raw.bits);
    TEST_ASSERT_TRUE(cmd.args.raw.expect_reply);

    /* An odd digit count would shift the opcode into the address byte. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("raw", "{\"frame\":\"FF080\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("raw", "{\"frame\":\"FFZ8\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("raw", "{\"frame\":65288}", &cmd, err, sizeof(err)));
}

static void test_query_takes_a_name_or_an_opcode(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("query", "{\"addr\":3,\"query\":\"actual_level\"}", &cmd,
                                        err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_QUERY, cmd.kind);
    TEST_ASSERT_EQUAL(GW_TARGET_SHORT, cmd.target.type);
    TEST_ASSERT_EQUAL_STRING("actual_level", cmd.args.query.name);
    TEST_ASSERT_EQUAL(-1, cmd.args.query.opcode);

    TEST_ASSERT_EQUAL(ESP_OK,
                      parse_cmd("query", "{\"addr\":3,\"opcode\":160}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(160, cmd.args.query.opcode);
    TEST_ASSERT_EQUAL_STRING("", cmd.args.query.name);

    /* Both or neither is ambiguous: the bus would have to guess which one wins. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("query", "{\"addr\":3,\"query\":\"status\",\"opcode\":160}", &cmd,
                                err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("query", "{\"addr\":3}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("query", "{\"query\":\"status\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_cmd("query", "{\"addr\":64,\"query\":\"status\"}",
                                                     &cmd, err, sizeof(err)));

    TEST_ASSERT_EQUAL(
        ESP_OK, parse_cmd("query", "{\"group\":2,\"query\":\"status\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_TARGET_GROUP, cmd.target.type);
    TEST_ASSERT_EQUAL(2, cmd.target.addr);
}

static void test_configure_builds_the_field_mask(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("configure",
                                        "{\"addr\":3,\"min\":85,\"fade_time\":4,"
                                        "\"scene\":{\"2\":128,\"3\":null},"
                                        "\"group\":{\"add\":[0],\"remove\":[5]}}",
                                        &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_CONFIGURE, cmd.kind);
    TEST_ASSERT_EQUAL(3, cmd.target.addr);

    const gw_configure_args_t *cfg = &cmd.args.configure;
    uint32_t expect =
        GW_CFG_FIELD_MIN | GW_CFG_FIELD_FADE_TIME | GW_CFG_FIELD_SCENES | GW_CFG_FIELD_GROUPS;
    TEST_ASSERT_EQUAL_HEX32(expect, cfg->fields);
    TEST_ASSERT_EQUAL(85, cfg->min);
    TEST_ASSERT_EQUAL(4, cfg->fade_time);

    /* Only the scenes actually named are touched; null means "clear", not "level 0". */
    TEST_ASSERT_EQUAL(GW_SCENE_UNTOUCHED, cfg->scenes[0]);
    TEST_ASSERT_EQUAL(GW_SCENE_UNTOUCHED, cfg->scenes[1]);
    TEST_ASSERT_EQUAL(128, cfg->scenes[2]);
    TEST_ASSERT_EQUAL(GW_SCENE_CLEAR, cfg->scenes[3]);

    TEST_ASSERT_EQUAL_HEX16(1u << 0, cfg->group_add);
    TEST_ASSERT_EQUAL_HEX16(1u << 5, cfg->group_remove);
}

static void test_configure_rejects_contradictions(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("configure", "{\"addr\":3}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("configure", "{\"addr\":3,\"group\":{\"add\":[2],\"remove\":[2]}}",
                                &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        parse_cmd("configure", "{\"addr\":3,\"scene\":{\"16\":10}}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_cmd("configure", "{\"addr\":3,\"fade_time\":16}",
                                                     &cmd, err, sizeof(err)));
}

static void test_addressing_commands(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("set_short_address", "{\"addr\":3,\"new_addr\":7}", &cmd,
                                        err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_SET_SHORT_ADDRESS, cmd.kind);
    TEST_ASSERT_EQUAL(3, cmd.target.addr);
    TEST_ASSERT_EQUAL(7, cmd.args.set_short_address.new_addr);

    TEST_ASSERT_EQUAL(ESP_OK,
                      parse_cmd("remove_short_address", "{\"addr\":3}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_REMOVE_SHORT_ADDRESS, cmd.kind);

    /* Re-addressing needs one gear, so a group target is not a valid spelling of it. */
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        parse_cmd("set_short_address", "{\"group\":1,\"new_addr\":7}", &cmd, err, sizeof(err)));

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("identify", "{\"addr\":3}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(GW_CMD_IDENTIFY, cmd.kind);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_cmd("identify", "{}", &cmd, err, sizeof(err)));
}

/* The id is echoed through a uint32_t, so a non-numeric one could never reach the client again. */
static void test_cmd_id_accepts_numbers_and_numeric_strings(void)
{
    gw_cmd_t cmd;
    char err[96];

    TEST_ASSERT_EQUAL(ESP_OK, parse_cmd("scan", "{\"id\":\"42\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(42, cmd.id);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("scan", "{\"id\":\"abc\"}", &cmd, err, sizeof(err)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_cmd("scan", "{\"id\":1.5}", &cmd, err, sizeof(err)));
}

void test_gw_api_wire_run(void)
{
    RUN_TEST(test_gear_object_has_the_spec_shape);
    RUN_TEST(test_gear_deep_adds_config_and_identity);
    RUN_TEST(test_gear_omits_blocks_that_were_never_read);
    RUN_TEST(test_level_pct_is_linear_at_the_edges);
    RUN_TEST(test_level_pct_round_trips);
    RUN_TEST(test_gears_list_is_compact);
    RUN_TEST(test_bus_reports_null_when_idle);
    RUN_TEST(test_bus_reports_progress_while_scanning);
    RUN_TEST(test_progress_event);
    RUN_TEST(test_result_ok_carries_data);
    RUN_TEST(test_result_error_carries_message_and_target);
    RUN_TEST(test_result_broadcast_target_has_no_addr);
    RUN_TEST(test_set_accepts_the_bare_forms);
    RUN_TEST(test_set_accepts_the_json_forms);
    RUN_TEST(test_set_rejects_nonsense);
    RUN_TEST(test_cmd_scan_and_cancel);
    RUN_TEST(test_commission_all_needs_confirm);
    RUN_TEST(test_raw_frame_is_4_or_6_hex_digits);
    RUN_TEST(test_query_takes_a_name_or_an_opcode);
    RUN_TEST(test_configure_builds_the_field_mask);
    RUN_TEST(test_configure_rejects_contradictions);
    RUN_TEST(test_addressing_commands);
    RUN_TEST(test_cmd_id_accepts_numbers_and_numeric_strings);
}

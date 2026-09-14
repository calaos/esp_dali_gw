/** esp-mqtt adapter: client lifecycle, topic routing, LWT, retained state and Home Assistant
 * discovery (SPEC 8). */
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "app_config.h"
#include "dali_bus.h"
#include "gw_api.h"
#include "gw_events.h"
#include "mqtt_iface.h"
#include "net_wifi.h"

static const char *TAG = "mqtt";

#define WORK_QUEUE_DEPTH 16
#define WORKER_STACK 5120
#define WORKER_PRIO 4
#define WORKER_TICK_MS 1000

#define TOPIC_LEN (APP_CONFIG_TOPIC_LEN + 48)
#define MAX_RX_PAYLOAD 4096
#define STATUS_INTERVAL_MS 60000
/** A 64-address scan changes 64 gears in ~4 s; the compact list is coalesced instead of spammed. */
#define GEARS_MIN_INTERVAL_MS 2000

/**
 * Bus faults arrive in bursts — a scan over an unpowered bus produces one line per address. Five
 * lines get through at once so the first failure is visible, then one every two seconds so a
 * persistent fault keeps reporting without flooding the broker or the work queue.
 */
#define LOG_BURST 5
#define LOG_REFILL_MS 2000

typedef enum {
    WORK_START,
    WORK_STOP,
    WORK_RESTART,
    WORK_CONNECTED,
    WORK_DISCONNECTED,
    WORK_MESSAGE,
    WORK_PUBLISH_ALL,
    WORK_BUS,
    WORK_GEAR,
    WORK_RESULT,
    WORK_PROGRESS,
    WORK_RX,
    WORK_LOG,
} work_kind_t;

typedef struct {
    work_kind_t kind;
    union {
        struct {
            char *topic;
            char *payload;
            size_t len;
        } msg;
        gw_event_gear_t gear;
        gw_event_result_t result;
        gw_event_progress_t progress;
        gw_event_rx_t rx;
        gw_event_log_t log;
    } u;
} work_t;

/* The client handle is touched by the worker task alone, which is why every lifecycle entry point
 * posts work instead of acting in the caller's context: no lock, and no HTTP task ever waits for an
 * in-flight publish. */
static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static volatile bool s_connected;
static bool s_events_bound;

static char s_base[APP_CONFIG_TOPIC_LEN];
static char s_uri[APP_CONFIG_URI_LEN];
static char s_username[APP_CONFIG_NAME_LEN];
static char s_password[APP_CONFIG_PASSWORD_LEN];
static char s_client_id[APP_CONFIG_NAME_LEN];
static char s_lwt_topic[TOPIC_LEN];
static char s_lwt_msg[32];
static int s_qos;
static bool s_retain;

static int64_t s_last_status_ms;
static int64_t s_last_gears_ms;
static bool s_gears_dirty;

/* Reassembly state for MQTT_EVENT_DATA, touched by the esp-mqtt task only. */
static char s_rx_topic[TOPIC_LEN];
static char *s_rx_buf;
static size_t s_rx_len;
static size_t s_rx_total;

/* Log rate limiter, touched by the default event loop task only. */
static int s_log_tokens = LOG_BURST;
static int64_t s_log_refill_ms;
static uint32_t s_log_dropped;

static void publish_gear(uint8_t addr);
static void publish_gears(void);

/* --- work queue ------------------------------------------------------------------------------ */

static esp_err_t post_work(const work_t *w, TickType_t wait)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_queue, w, wait) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static void work_free(work_t *w)
{
    if (w->kind == WORK_MESSAGE) {
        free(w->u.msg.topic);
        free(w->u.msg.payload);
        w->u.msg.topic = NULL;
        w->u.msg.payload = NULL;
    }
}

/* --- publishing ------------------------------------------------------------------------------ */

/** Takes ownership of @p json in every case, so a failed build cannot leak. */
static void publish_json(const char *topic, cJSON *json, bool retain)
{
    if (json == NULL) {
        ESP_LOGE(TAG, "could not build payload for %s", topic);
        return;
    }
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (text == NULL) {
        return;
    }
    if (s_client != NULL &&
        esp_mqtt_client_publish(s_client, topic, text, 0, s_qos, retain ? 1 : 0) < 0) {
        ESP_LOGW(TAG, "publish %s failed", topic);
    }
    cJSON_free(text);
}

static void publish_result(const char *action, uint32_t id, bool ok, gw_err_t error,
                           const char *message, cJSON *data)
{
    gw_result_t res = {0};
    res.id = id;
    strlcpy(res.action, action, sizeof(res.action));
    res.ok = ok;
    res.error = error;
    if (message != NULL) {
        strlcpy(res.message, message, sizeof(res.message));
    }
    res.data = data;

    char topic[TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/result/%s", s_base, res.action);
    publish_json(topic, gw_api_result_to_json(&res), false);
    gw_api_result_free(&res);
}

/* gw_api drops `data` on a failed result, so the offending topic travels in the message instead. */
static void publish_topic_error(const char *topic, const char *reason)
{
    char message[96];
    snprintf(message, sizeof(message), "%s: %s", topic, reason);
    publish_result("error", 0, false, GW_ERR_INVALID_ARG, message, NULL);
}

static void collect_info(gw_info_t *info)
{
    memset(info, 0, sizeof(*info));

    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(info->state, "online", sizeof(info->state));
    strlcpy(info->fw, app->version, sizeof(info->fw));
    strlcpy(info->idf, IDF_VER, sizeof(info->idf));

    net_wifi_status_t net;
    net_wifi_get_status(&net);
    strlcpy(info->ip, net.ip, sizeof(info->ip));
    strlcpy(info->hostname, net.hostname, sizeof(info->hostname));
    strlcpy(info->mode, net.mode, sizeof(info->mode));
    info->rssi = net.rssi;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(info->mac, sizeof(info->mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    app_config_device_id(info->device_id, sizeof(info->device_id));

    info->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    info->free_heap = esp_get_free_heap_size();
    info->min_free_heap = esp_get_minimum_free_heap_size();
    info->reset_reason = (int)esp_reset_reason();
}

static void publish_status(void)
{
    gw_info_t info;
    collect_info(&info);

    char topic[TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/status", s_base);
    publish_json(topic, gw_api_status_to_json(&info), s_retain);
    s_last_status_ms = esp_timer_get_time() / 1000;
}

static void publish_bus(void)
{
    gw_bus_status_t bus;
    dali_bus_get_status(&bus);

    char topic[TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/bus", s_base);
    publish_json(topic, gw_api_bus_to_json(&bus), s_retain);
}

static void publish_gear(uint8_t addr)
{
    gw_gear_t gear;
    if (dali_bus_get_gear(addr, &gear) != ESP_OK) {
        return;
    }
    char topic[TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/gear/%u/state", s_base, (unsigned)addr);
    publish_json(topic, gw_api_gear_to_json(&gear, false), s_retain);
}

/** The registry snapshot is ~10 KB; it lives on the heap for the call rather than in BSS. */
static void publish_gears(void)
{
    gw_gear_t *gears = calloc(GW_MAX_GEARS, sizeof(*gears));
    if (gears == NULL) {
        ESP_LOGE(TAG, "no memory for the gear list");
        return;
    }
    size_t count = 0;
    if (dali_bus_get_gears(gears, GW_MAX_GEARS, &count) == ESP_OK) {
        char topic[TOPIC_LEN];
        snprintf(topic, sizeof(topic), "%s/gears", s_base);
        publish_json(topic, gw_api_gears_to_json(gears, count), s_retain);
    }
    free(gears);

    s_gears_dirty = false;
    s_last_gears_ms = esp_timer_get_time() / 1000;
}

static void publish_every_gear_state(void)
{
    gw_gear_t *gears = calloc(GW_MAX_GEARS, sizeof(*gears));
    if (gears == NULL) {
        ESP_LOGE(TAG, "no memory for the gear list");
        return;
    }
    size_t count = 0;
    if (dali_bus_get_gears(gears, GW_MAX_GEARS, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            char topic[TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%s/gear/%u/state", s_base, (unsigned)gears[i].addr);
            publish_json(topic, gw_api_gear_to_json(&gears[i], false), s_retain);
        }
    }
    free(gears);
}

static void publish_all(void)
{
    publish_status();
    publish_bus();
    publish_every_gear_state();
    publish_gears();
}

/* --- Home Assistant discovery (SPEC 8.4, ADR 0005) -------------------------------------------- */

#define HA_OBJECT_ID_LEN 32
#define HA_TOPIC_LEN (APP_CONFIG_NAME_LEN + HA_OBJECT_ID_LEN + 16)

/*
 * The documents use Home Assistant's *template* light schema rather than the JSON schema named in
 * SPEC 8.4. The JSON schema has no state_value_template — it expects its own `state`/`brightness`
 * keys in the state payload and commands with them too, neither of which the gear object and the
 * /set payload of SPEC 8.1-8.2 use. ADR 0005 has the reasoning; the templates below are the whole
 * of the mapping. Brightness is the raw DALI level, never level_pct (ADR 0003).
 */
#define HA_TPL_STATE                                                                               \
    "{% if value_json.on is none %}None{% elif value_json.on %}on{% else %}off{% endif %}"
#define HA_TPL_BRIGHTNESS "{{ value_json.level | default('', true) }}"
/* The gear object carries no colour readback yet: these render empty, which HA ignores. They are
 * still what declares colour support, so the controls exist and start reporting for free the day
 * the registry gains the fields. */
#define HA_TPL_COLOR_TEMP "{{ value_json.mirek | default('', true) }}"
#define HA_TPL_RED "{{ (value_json.rgb | default([]))[0] | default('', true) }}"
#define HA_TPL_GREEN "{{ (value_json.rgb | default([]))[1] | default('', true) }}"
#define HA_TPL_BLUE "{{ (value_json.rgb | default([]))[2] | default('', true) }}"
/* The LWT publishes {"state":"offline"} on the status topic, so availability reads one member. */
#define HA_TPL_AVAILABILITY "{{ value_json.state }}"
/* One service call is one message, and a /set payload carries one directive (gw_api precedence:
 * level first). Colour is tested first here so that an explicit colour request is never the one
 * dropped when HA sends brightness alongside it. */
#define HA_TPL_COMMAND_ON                                                                          \
    "{% if color_temp is defined %}{\"mirek\": {{ color_temp }}}"                                  \
    "{% elif red is defined %}{\"rgb\": [{{ [red, 254] | min }}, {{ [green, 254] | min }}, "       \
    "{{ [blue, 254] | min }}]}"                                                                    \
    "{% elif brightness is defined %}{\"level\": {{ [brightness, 254] | min }}}"                   \
    "{% else %}{\"on\": true}{% endif %}"
#define HA_TPL_COMMAND_OFF "{\"on\": false}"

/** Prefix the retained documents were published under; empty while discovery is off. */
static char s_ha_prefix[APP_CONFIG_NAME_LEN];
static char s_ha_device[24]; /**< "esp_dali_gw_<id>", also the device identifier */
static uint64_t s_ha_gears;  /**< bit n: a config document is retained for short address n */
static uint16_t s_ha_groups;
static bool s_ha_swept;

static void ha_object_id(char *out, size_t len, bool group, uint8_t index)
{
    if (s_ha_device[0] == '\0') {
        char id[8];
        app_config_device_id(id, sizeof(id));
        snprintf(s_ha_device, sizeof(s_ha_device), "esp_dali_gw_%s", id);
    }
    if (group) {
        snprintf(out, len, "%s_g%u", s_ha_device, (unsigned)index);
    } else {
        snprintf(out, len, "%s_%u", s_ha_device, (unsigned)index);
    }
}

/** Discovery is retained whatever mqtt.retain_state says: HA reads it when *it* starts, not us. */
static void ha_publish_doc(const char *object_id, cJSON *doc)
{
    char topic[HA_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/light/%s/config", s_ha_prefix, object_id);
    publish_json(topic, doc, true);
}

static void ha_clear_doc(const char *object_id)
{
    char topic[HA_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/light/%s/config", s_ha_prefix, object_id);
    if (s_client != NULL && esp_mqtt_client_publish(s_client, topic, "", 0, s_qos, 1) < 0) {
        ESP_LOGW(TAG, "could not remove %s", topic);
    }
}

static cJSON *ha_device_block(void)
{
    cJSON *dev = cJSON_CreateObject();
    if (dev == NULL) {
        return NULL;
    }
    const char *ident = s_ha_device;
    cJSON_AddItemToObject(dev, "identifiers", cJSON_CreateStringArray(&ident, 1));
    cJSON_AddStringToObject(dev, "name", app_config_get()->device.name);
    cJSON_AddStringToObject(dev, "model", "ESP DALI GW");
    cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);

    net_wifi_status_t net;
    net_wifi_get_status(&net);
    if (net.ip[0] != '\0' && strcmp(net.ip, "0.0.0.0") != 0) {
        char url[32];
        snprintf(url, sizeof(url), "http://%s/", net.ip);
        cJSON_AddStringToObject(dev, "configuration_url", url);
    }
    return dev;
}

/*
 * DT8 colour type features (Part 209): bit 1 = colour temperature, bits 5..7 = number of RGBWAF
 * channels. The scan does not read that byte yet, so zero means "unknown" and both are offered.
 */
static bool ha_dt8_has_tc(const gw_gear_dt8_t *dt8)
{
    return dt8->caps == 0 || (dt8->caps & 0x02) != 0;
}

static bool ha_dt8_has_rgb(const gw_gear_dt8_t *dt8)
{
    return dt8->caps == 0 || ((dt8->caps >> 5) & 0x07) >= 3;
}

/** @param state_topic  NULL for a group, which has no readback and is optimistic in HA. */
static cJSON *ha_light_doc(const char *object_id, const char *name, const char *command_topic,
                           const char *state_topic, const gw_gear_dt8_t *dt8)
{
    cJSON *doc = cJSON_CreateObject();
    if (doc == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(doc, "schema", "template");
    cJSON_AddStringToObject(doc, "name", name);
    cJSON_AddStringToObject(doc, "unique_id", object_id);
    cJSON_AddStringToObject(doc, "command_topic", command_topic);
    cJSON_AddStringToObject(doc, "command_on_template", HA_TPL_COMMAND_ON);
    cJSON_AddStringToObject(doc, "command_off_template", HA_TPL_COMMAND_OFF);
    /* Present even for a group: it is what gives the entity a brightness slider at all. */
    cJSON_AddStringToObject(doc, "brightness_template", HA_TPL_BRIGHTNESS);
    if (state_topic != NULL) {
        cJSON_AddStringToObject(doc, "state_topic", state_topic);
        cJSON_AddStringToObject(doc, "state_template", HA_TPL_STATE);
    }

    char availability[TOPIC_LEN];
    snprintf(availability, sizeof(availability), "%s/status", s_base);
    cJSON_AddStringToObject(doc, "availability_topic", availability);
    cJSON_AddStringToObject(doc, "availability_template", HA_TPL_AVAILABILITY);
    cJSON_AddNumberToObject(doc, "qos", s_qos);

    if (dt8 != NULL && dt8->supported) {
        if (ha_dt8_has_tc(dt8)) {
            cJSON_AddStringToObject(doc, "color_temp_template", HA_TPL_COLOR_TEMP);
            if (dt8->tc_min_mirek != 0 && dt8->tc_max_mirek != 0) {
                cJSON_AddNumberToObject(doc, "min_mireds", dt8->tc_min_mirek);
                cJSON_AddNumberToObject(doc, "max_mireds", dt8->tc_max_mirek);
            }
        }
        if (ha_dt8_has_rgb(dt8)) {
            cJSON_AddStringToObject(doc, "red_template", HA_TPL_RED);
            cJSON_AddStringToObject(doc, "green_template", HA_TPL_GREEN);
            cJSON_AddStringToObject(doc, "blue_template", HA_TPL_BLUE);
        }
    }
    cJSON_AddItemToObject(doc, "device", ha_device_block());
    return doc;
}

/**
 * @brief Publish or remove the document for one gear.
 *
 * @param sweep  also remove a document this session never published — see ha_sync().
 */
static void ha_sync_gear(uint8_t addr, bool sweep)
{
    if (s_ha_prefix[0] == '\0' || addr >= GW_MAX_GEARS) {
        return;
    }
    char object_id[HA_OBJECT_ID_LEN];
    ha_object_id(object_id, sizeof(object_id), false, addr);
    const uint64_t bit = 1ULL << addr;

    gw_gear_t gear;
    if (dali_bus_get_gear(addr, &gear) != ESP_OK || !gear.present) {
        if (sweep || (s_ha_gears & bit) != 0) {
            ha_clear_doc(object_id);
            s_ha_gears &= ~bit;
        }
        return;
    }

    char name[GW_NAME_LEN + 8];
    if (gear.name[0] != '\0') {
        strlcpy(name, gear.name, sizeof(name));
    } else {
        snprintf(name, sizeof(name), "Gear %u", (unsigned)addr);
    }
    char command_topic[TOPIC_LEN];
    char state_topic[TOPIC_LEN];
    snprintf(command_topic, sizeof(command_topic), "%s/gear/%u/set", s_base, (unsigned)addr);
    snprintf(state_topic, sizeof(state_topic), "%s/gear/%u/state", s_base, (unsigned)addr);

    ha_publish_doc(object_id, ha_light_doc(object_id, name, command_topic, state_topic, &gear.dt8));
    s_ha_gears |= bit;
}

/** Groups every present gear reports. Zero until a deep scan has read the membership bits. */
static uint16_t ha_group_members(void)
{
    uint16_t mask = 0;
    for (uint8_t addr = 0; addr < GW_MAX_GEARS; addr++) {
        gw_gear_t gear;
        if (dali_bus_get_gear(addr, &gear) == ESP_OK && gear.present && gear.config.valid) {
            mask |= gear.config.groups;
        }
    }
    return mask;
}

/** A group is worth an entity once something is known to be in it, or once someone has named it. */
static void ha_sync_group(uint8_t group, uint16_t members, bool sweep)
{
    if (s_ha_prefix[0] == '\0' || group >= GW_MAX_GROUPS) {
        return;
    }
    char object_id[HA_OBJECT_ID_LEN];
    ha_object_id(object_id, sizeof(object_id), true, group);
    const uint16_t bit = (uint16_t)(1u << group);

    const char *named = app_config_group_name(group);
    if (named == NULL && (members & bit) == 0) {
        if (sweep || (s_ha_groups & bit) != 0) {
            ha_clear_doc(object_id);
            s_ha_groups &= (uint16_t)~bit;
        }
        return;
    }

    char name[GW_NAME_LEN + 8];
    if (named != NULL) {
        strlcpy(name, named, sizeof(name));
    } else {
        snprintf(name, sizeof(name), "Group %u", (unsigned)group);
    }
    char command_topic[TOPIC_LEN];
    snprintf(command_topic, sizeof(command_topic), "%s/group/%u/set", s_base, (unsigned)group);

    ha_publish_doc(object_id, ha_light_doc(object_id, name, command_topic, NULL, NULL));
    s_ha_groups |= bit;
}

static void ha_clear_all(void)
{
    for (uint8_t addr = 0; addr < GW_MAX_GEARS; addr++) {
        if ((s_ha_gears & (1ULL << addr)) != 0) {
            char object_id[HA_OBJECT_ID_LEN];
            ha_object_id(object_id, sizeof(object_id), false, addr);
            ha_clear_doc(object_id);
        }
    }
    for (uint8_t group = 0; group < GW_MAX_GROUPS; group++) {
        if ((s_ha_groups & (1u << group)) != 0) {
            char object_id[HA_OBJECT_ID_LEN];
            ha_object_id(object_id, sizeof(object_id), true, group);
            ha_clear_doc(object_id);
        }
    }
    s_ha_gears = 0;
    s_ha_groups = 0;
}

/**
 * @brief Reconcile every discovery document with the registry.
 *
 * Called on connect and after a scan or commissioning — never on a gear state change, which would
 * republish 64 documents every time someone dims a light. The documents are retained, so an HA
 * restart needs nothing from us; a broker that lost its retained store, a new IP in
 * configuration_url and a changed base topic are what the republish on connect is for.
 */
static void ha_sync(void)
{
    const app_config_t *cfg = app_config_get();
    const char *prefix = cfg->mqtt.ha_discovery.enabled ? cfg->mqtt.ha_discovery.prefix : "";

    /* A retained document only exists under the prefix it was published with, so a changed — or
     * switched off — prefix has to be emptied before the new one is adopted, or its entities stay
     * in Home Assistant for ever. */
    if (s_ha_prefix[0] != '\0' && strcmp(prefix, s_ha_prefix) != 0) {
        ha_clear_all();
        s_ha_swept = false;
    }
    strlcpy(s_ha_prefix, prefix, sizeof(s_ha_prefix));
    if (prefix[0] == '\0') {
        return;
    }

    gw_bus_status_t bus;
    dali_bus_get_status(&bus);
    /* Documents left retained by a previous boot can only be removed blind, and only once a scan
     * has established which gears really answer: before that the registry is empty and clearing
     * every address would drop the entities of a gateway that merely rebooted. Afterwards the
     * advertised masks are authoritative, so the blind pass runs exactly once. */
    const bool sweep = !s_ha_swept && bus.last_scan != 0;

    for (uint8_t addr = 0; addr < GW_MAX_GEARS; addr++) {
        ha_sync_gear(addr, sweep);
    }
    const uint16_t members = ha_group_members();
    for (uint8_t group = 0; group < GW_MAX_GROUPS; group++) {
        ha_sync_group(group, members, sweep);
    }
    s_ha_swept = s_ha_swept || sweep;
}

/* --- inbound routing ------------------------------------------------------------------------- */

static const char *action_name(gw_cmd_kind_t kind)
{
    switch (kind) {
        case GW_CMD_SET_LEVEL:
            return "set_level";
        case GW_CMD_ON:
            return "on";
        case GW_CMD_OFF:
            return "off";
        case GW_CMD_INDIRECT:
            return "cmd";
        case GW_CMD_COLOR:
            return "color";
        case GW_CMD_QUERY:
            return "query";
        case GW_CMD_CONFIGURE:
            return "configure";
        case GW_CMD_RAW:
            return "raw";
        case GW_CMD_IDENTIFY:
            return "identify";
        case GW_CMD_SET_SHORT_ADDRESS:
            return "set_short_address";
        case GW_CMD_REMOVE_SHORT_ADDRESS:
            return "remove_short_address";
        case GW_CMD_SCAN:
            return "scan";
        case GW_CMD_COMMISSION:
            return "commission";
        case GW_CMD_POLL_ALL:
            return "poll_all";
        case GW_CMD_BUS_CHECK:
            return "bus_check";
        case GW_CMD_CANCEL:
            return "cancel";
        case GW_CMD_NONE:
            break;
    }
    return "unknown";
}

static bool is_long_operation(gw_cmd_kind_t kind)
{
    return kind == GW_CMD_SCAN || kind == GW_CMD_COMMISSION || kind == GW_CMD_POLL_ALL;
}

static void submit(const gw_cmd_t *cmd, const char *action)
{
    esp_err_t err = dali_bus_submit(cmd);
    if (err == ESP_ERR_NO_MEM) {
        publish_result(action, cmd->id, false, GW_ERR_BUS_BUSY, "bus command queue full", NULL);
        return;
    }
    if (err != ESP_OK) {
        publish_result(action, cmd->id, false, gw_api_err_from_esp(err),
                       "could not enqueue command", NULL);
        return;
    }
    /* SPEC 8.3: a long operation acknowledges its start; progress and the final result follow. */
    if (is_long_operation(cmd->kind)) {
        cJSON *data = cJSON_CreateObject();
        if (data != NULL) {
            cJSON_AddBoolToObject(data, "started", true);
        }
        publish_result(action, cmd->id, true, GW_OK, NULL, data);
    }
}

static bool parse_index(const char *s, size_t n, unsigned max, uint8_t *out)
{
    if (n == 0 || n > 2) {
        return false;
    }
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v > max) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

/** The "id" of a rejected payload still has to be echoed, so it is read before gw_api parses. */
static uint32_t payload_id(const cJSON *root)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id) && id->valuedouble >= 0) {
        return (uint32_t)id->valuedouble;
    }
    if (cJSON_IsString(id) && id->valuestring != NULL) {
        return (uint32_t)strtoul(id->valuestring, NULL, 10);
    }
    return 0;
}

static void handle_set(const char *topic, gw_target_t target, const char *payload, size_t len)
{
    gw_cmd_t cmd = {0};
    char err[96] = {0};

    if (gw_api_set_from_payload(payload, len, target, &cmd, err, sizeof(err)) != ESP_OK) {
        publish_topic_error(topic, err[0] != '\0' ? err : "invalid set payload");
        return;
    }
    cmd.origin = GW_ORIGIN_MQTT;
    submit(&cmd, action_name(cmd.kind));
}

static void cmd_get_gears(uint32_t id)
{
    publish_every_gear_state();
    publish_gears();
    publish_result("get_gears", id, true, GW_OK, NULL, NULL);
}

static void cmd_rename(const cJSON *root, uint32_t id)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *addr = cJSON_GetObjectItemCaseSensitive(root, "addr");
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group");

    if (!cJSON_IsString(name) || (!cJSON_IsNumber(addr) && !cJSON_IsNumber(group))) {
        publish_result("rename", id, false, GW_ERR_INVALID_ARG,
                       "expected {\"addr\"|\"group\": n, \"name\": \"...\"}", NULL);
        return;
    }

    esp_err_t err;
    if (cJSON_IsNumber(addr)) {
        int a = addr->valueint;
        if (a < 0 || a >= GW_MAX_GEARS) {
            publish_result("rename", id, false, GW_ERR_INVALID_ARG, "addr out of range", NULL);
            return;
        }
        err = app_config_set_gear_name((uint8_t)a, name->valuestring);
        if (err == ESP_OK) {
            dali_bus_set_gear_name((uint8_t)a, name->valuestring);
            publish_gear((uint8_t)a);
            ha_sync_gear((uint8_t)a, false);
            s_gears_dirty = true;
        }
    } else {
        int g = group->valueint;
        if (g < 0 || g >= GW_MAX_GROUPS) {
            publish_result("rename", id, false, GW_ERR_INVALID_ARG, "group out of range", NULL);
            return;
        }
        err = app_config_set_group_name((uint8_t)g, name->valuestring);
        if (err == ESP_OK) {
            ha_sync_group((uint8_t)g, ha_group_members(), false);
        }
    }

    publish_result("rename", id, err == ESP_OK, gw_api_err_from_esp(err),
                   err == ESP_OK ? NULL : "could not persist the name", NULL);
}

static void cmd_get_config(uint32_t id)
{
    publish_result("get_config", id, true, GW_OK, NULL,
                   gw_api_config_to_json(app_config_get(), false));
}

/* Mirrors apply_config() in http_routes.c: the two adapters must not diverge on what a write does.
 */
static void cmd_set_config(const cJSON *root, uint32_t id)
{
    app_config_t cfg = *app_config_get();
    char field[64] = {0};

    esp_err_t err = gw_api_config_from_json(root, &cfg, field, sizeof(field));
    if (err != ESP_OK) {
        publish_result("set_config", id, false, GW_ERR_INVALID_ARG,
                       field[0] ? field : "malformed document", NULL);
        return;
    }

    app_config_merge_secrets(&cfg);

    err = app_config_validate(&cfg, field, sizeof(field));
    if (err != ESP_OK) {
        publish_result("set_config", id, false, GW_ERR_INVALID_ARG,
                       field[0] ? field : "invalid value", NULL);
        return;
    }

    app_config_impact_t impact = APP_CONFIG_IMPACT_NONE;
    err = app_config_set(&cfg, &impact);
    if (err != ESP_OK) {
        publish_result("set_config", id, false, gw_api_err_from_esp(err),
                       "could not persist configuration", NULL);
        return;
    }

    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddBoolToObject(data, "reboot_required", (impact & APP_CONFIG_IMPACT_REBOOT) != 0);
        cJSON_AddBoolToObject(data, "mqtt_restart", (impact & APP_CONFIG_IMPACT_MQTT_RESTART) != 0);
    }
    publish_result("set_config", id, true, GW_OK, NULL, data);

    /* Published first: restarting the client here would drop the reply the caller is waiting for.
     */
    if (impact & APP_CONFIG_IMPACT_MQTT_RESTART) {
        mqtt_iface_restart();
    }
}

static void cmd_reboot(const char *action, const cJSON *root, uint32_t id)
{
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (!cJSON_IsTrue(confirm)) {
        publish_result(action, id, false, GW_ERR_INVALID_ARG, "confirm must be true", NULL);
        return;
    }

    if (strcmp(action, "factory_reset") == 0) {
        esp_err_t err = app_config_factory_reset();
        if (err != ESP_OK) {
            publish_result(action, id, false, gw_api_err_from_esp(err),
                           "could not erase configuration", NULL);
            return;
        }
    }
    publish_result(action, id, true, GW_OK, NULL, NULL);
    ESP_LOGW(TAG, "%s requested over MQTT", action);

    /* Own task, not a callback: the delay just lets the reply reach the broker before the reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/** @retval true the adapter answered this action itself and the bus was not involved. */
static bool handle_local_cmd(const char *action, const cJSON *root, uint32_t id)
{
    if (strcmp(action, "get_gears") == 0) {
        cmd_get_gears(id);
    } else if (strcmp(action, "rename") == 0) {
        cmd_rename(root, id);
    } else if (strcmp(action, "get_config") == 0) {
        cmd_get_config(id);
    } else if (strcmp(action, "set_config") == 0) {
        cmd_set_config(root, id);
    } else if (strcmp(action, "reboot") == 0 || strcmp(action, "factory_reset") == 0) {
        cmd_reboot(action, root, id);
    } else {
        return false;
    }
    return true;
}

static void handle_cmd(const char *action, const char *payload, size_t len)
{
    cJSON *root = (len > 0) ? cJSON_ParseWithLength(payload, len) : NULL;
    if (len > 0 && root == NULL) {
        publish_result(action, 0, false, GW_ERR_INVALID_ARG, "payload is not valid JSON", NULL);
        return;
    }
    uint32_t id = payload_id(root);

    if (!handle_local_cmd(action, root, id)) {
        gw_cmd_t cmd = {0};
        char err[96] = {0};
        if (gw_api_cmd_from_json(action, root, &cmd, err, sizeof(err)) != ESP_OK) {
            publish_result(action, id, false, GW_ERR_INVALID_ARG,
                           err[0] ? err : "unknown command or arguments", NULL);
        } else {
            cmd.origin = GW_ORIGIN_MQTT;
            submit(&cmd, action);
        }
    }
    cJSON_Delete(root);
}

static void route_message(const char *topic, const char *payload, size_t len)
{
    size_t base_len = strlen(s_base);
    if (strncmp(topic, s_base, base_len) != 0 || topic[base_len] != '/') {
        publish_topic_error(topic, "not under the configured base topic");
        return;
    }
    const char *tail = topic + base_len + 1;
    gw_target_t target = {.type = GW_TARGET_BROADCAST, .addr = 0};

    if (strncmp(tail, "cmd/", 4) == 0) {
        const char *action = tail + 4;
        if (action[0] == '\0' || strchr(action, '/') != NULL) {
            publish_topic_error(topic, "unknown topic");
            return;
        }
        handle_cmd(action, payload, len);
        return;
    }

    if (strncmp(tail, "gear/", 5) == 0 || strncmp(tail, "group/", 6) == 0) {
        bool gear = tail[0] == 'g' && tail[1] == 'e';
        const char *index = tail + (gear ? 5 : 6);
        const char *slash = strchr(index, '/');
        if (slash == NULL || strcmp(slash, "/set") != 0 ||
            !parse_index(index, (size_t)(slash - index),
                         gear ? GW_MAX_GEARS - 1 : GW_MAX_GROUPS - 1, &target.addr)) {
            publish_topic_error(topic, "address out of range");
            return;
        }
        target.type = gear ? GW_TARGET_SHORT : GW_TARGET_GROUP;
    } else if (strcmp(tail, "broadcast/set") != 0) {
        publish_topic_error(topic, "unknown topic");
        return;
    }

    handle_set(topic, target, payload, len);
}

/* --- client lifecycle ------------------------------------------------------------------------ */

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

static void client_stop(void)
{
    if (s_client == NULL) {
        return;
    }
    esp_mqtt_client_handle_t client = s_client;
    s_client = NULL;
    s_connected = false;

    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    ESP_LOGI(TAG, "client stopped");
}

static esp_err_t client_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;
    }
    const app_config_t *cfg = app_config_get();

    strlcpy(s_base, cfg->mqtt.base_topic, sizeof(s_base));
    size_t base_len = strlen(s_base);
    while (base_len > 0 && s_base[base_len - 1] == '/') {
        s_base[--base_len] = '\0';
    }
    if (base_len == 0) {
        ESP_LOGE(TAG, "mqtt.base_topic is empty, not starting");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_uri, cfg->mqtt.uri, sizeof(s_uri));
    strlcpy(s_username, cfg->mqtt.username, sizeof(s_username));
    strlcpy(s_password, cfg->mqtt.password, sizeof(s_password));
    strlcpy(s_client_id, cfg->mqtt.client_id, sizeof(s_client_id));
    s_qos = cfg->mqtt.qos;
    s_retain = cfg->mqtt.retain_state;
    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "%s/status", s_base);

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = s_uri,
        .credentials.username = s_username[0] != '\0' ? s_username : NULL,
        .credentials.client_id = s_client_id[0] != '\0' ? s_client_id : NULL,
        .credentials.authentication.password = s_password[0] != '\0' ? s_password : NULL,
        .session.last_will.topic = s_lwt_topic,
        .session.last_will.msg = s_lwt_msg,
        .session.last_will.msg_len = (int)strlen(s_lwt_msg),
        .session.last_will.qos = s_qos,
        .session.last_will.retain = s_retain ? 1 : 0,
        .session.keepalive = cfg->mqtt.keepalive_s,
        /* The configuration document is the largest payload either way: 1 KB is not enough. */
        .buffer.size = 2048,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mcfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed for %s", s_uri);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(client);
    }
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(client);
        ESP_LOGE(TAG, "could not start the client: %s", esp_err_to_name(err));
        return err;
    }

    s_client = client;
    ESP_LOGI(TAG, "connecting to %s, base topic %s", s_uri, s_base);
    return ESP_OK;
}

static void on_connected(void)
{
    static const char *const filters[] = {"gear/+/set", "group/+/set", "broadcast/set", "cmd/#"};

    for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
        char topic[TOPIC_LEN];
        snprintf(topic, sizeof(topic), "%s/%s", s_base, filters[i]);
        if (esp_mqtt_client_subscribe_single(s_client, topic, s_qos) < 0) {
            ESP_LOGW(TAG, "subscribe %s failed", topic);
        }
    }
    publish_all();
    ha_sync();
}

/* --- worker task ----------------------------------------------------------------------------- */

static void handle_work(work_t *w)
{
    switch (w->kind) {
        case WORK_START:
            client_start();
            break;
        case WORK_STOP:
            client_stop();
            break;
        case WORK_RESTART:
            client_stop();
            client_start();
            break;
        case WORK_CONNECTED:
            on_connected();
            break;
        case WORK_DISCONNECTED:
            break;
        case WORK_MESSAGE:
            route_message(w->u.msg.topic, w->u.msg.payload, w->u.msg.len);
            break;
        case WORK_PUBLISH_ALL:
            publish_all();
            break;
        case WORK_BUS:
            publish_bus();
            break;
        case WORK_GEAR:
            publish_gear(w->u.gear.addr);
            s_gears_dirty = true;
            /* A gear that turns up between two scans gets its entity straight away; one that is
             * already advertised is left alone, so dimming never republishes a document. */
            if (w->u.gear.addr < GW_MAX_GEARS && (s_ha_gears & (1ULL << w->u.gear.addr)) == 0) {
                ha_sync_gear(w->u.gear.addr, false);
            }
            break;
        case WORK_RESULT: {
            gw_result_t res = {0};
            res.id = w->u.result.id;
            strlcpy(res.action, w->u.result.action, sizeof(res.action));
            res.ok = w->u.result.ok;
            res.error = w->u.result.error;
            res.has_target = w->u.result.has_target;
            res.target = w->u.result.target;
            res.duration_ms = w->u.result.duration_ms;

            char topic[TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%s/result/%s", s_base,
                     res.action[0] != '\0' ? res.action : "unknown");
            publish_json(topic, gw_api_result_to_json(&res), false);

            /* A scan rewrites the whole registry, so the retained view is republished wholesale:
             * gear_count and last_scan move without a bus state change of their own. */
            if (strcmp(res.action, "scan") == 0 || strcmp(res.action, "commission") == 0) {
                publish_every_gear_state();
                publish_gears();
                publish_bus();
                ha_sync();
            }
            break;
        }
        case WORK_RX: {
            char topic[TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%s/event/rx", s_base);
            cJSON *obj = cJSON_CreateObject();
            if (obj != NULL) {
                char hex[9];
                snprintf(hex, sizeof(hex), "%0*" PRIX32, w->u.rx.bits / 4, w->u.rx.frame);
                cJSON_AddStringToObject(obj, "frame", hex);
                cJSON_AddNumberToObject(obj, "bits", w->u.rx.bits);
                cJSON_AddNumberToObject(obj, "ts", (double)(w->u.rx.timestamp_us / 1000));
            }
            publish_json(topic, obj, false);
            break;
        }
        case WORK_PROGRESS: {
            char topic[TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%s/event/progress", s_base);
            publish_json(topic,
                         gw_api_progress_to_json(w->u.progress.operation, w->u.progress.done,
                                                 w->u.progress.total, w->u.progress.found),
                         false);
            break;
        }
        case WORK_LOG: {
            cJSON *obj = cJSON_CreateObject();
            if (obj != NULL) {
                cJSON_AddStringToObject(obj, "level", w->u.log.level);
                cJSON_AddStringToObject(obj, "msg", w->u.log.msg);
            }
            char topic[TOPIC_LEN];
            snprintf(topic, sizeof(topic), "%s/event/log", s_base);
            publish_json(topic, obj, false);
            break;
        }
    }
    work_free(w);
}

static void worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        work_t w;
        if (xQueueReceive(s_queue, &w, pdMS_TO_TICKS(WORKER_TICK_MS)) == pdTRUE) {
            handle_work(&w);
        }
        if (!s_connected) {
            continue;
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_last_status_ms >= STATUS_INTERVAL_MS) {
            publish_status();
        }
        if (s_gears_dirty && now_ms - s_last_gears_ms >= GEARS_MIN_INTERVAL_MS) {
            publish_gears();
        }
    }
}

/* --- event plumbing -------------------------------------------------------------------------- */

/** Runs in the esp-mqtt task: copy what is needed, hand it to the worker, return. */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = data;
    work_t w = {0};

    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED: {
            s_connected = true;
            gw_event_mqtt_t state = {.connected = true};
            gw_event_post(GW_EVENT_MQTT_STATE, &state, sizeof(state));
            ESP_LOGI(TAG, "connected to %s", s_uri);
            w.kind = WORK_CONNECTED;
            post_work(&w, 0);
            break;
        }
        case MQTT_EVENT_DISCONNECTED: {
            s_connected = false;
            gw_event_mqtt_t state = {.connected = false};
            gw_event_post(GW_EVENT_MQTT_STATE, &state, sizeof(state));
            ESP_LOGW(TAG, "disconnected");
            w.kind = WORK_DISCONNECTED;
            post_work(&w, 0);
            break;
        }
        case MQTT_EVENT_DATA:
            /* Payloads longer than the input buffer arrive in fragments; reassemble before
             * routing so a large set_config is not seen as several malformed documents. */
            if (ev->current_data_offset == 0) {
                free(s_rx_buf);
                s_rx_buf = NULL;
                s_rx_len = 0;
                s_rx_total = 0;
                if (ev->topic_len <= 0 || (size_t)ev->topic_len >= sizeof(s_rx_topic)) {
                    ESP_LOGW(TAG, "dropping a message with an oversized topic");
                    break;
                }
                memcpy(s_rx_topic, ev->topic, (size_t)ev->topic_len);
                s_rx_topic[ev->topic_len] = '\0';
                if (ev->total_data_len < 0 || ev->total_data_len > MAX_RX_PAYLOAD) {
                    ESP_LOGW(TAG, "dropping a %d byte payload on %s", ev->total_data_len,
                             s_rx_topic);
                    break;
                }
                s_rx_total = (size_t)ev->total_data_len;
                s_rx_buf = malloc(s_rx_total + 1);
                if (s_rx_buf == NULL) {
                    break;
                }
            }
            if (s_rx_buf == NULL || s_rx_len + (size_t)ev->data_len > s_rx_total) {
                break;
            }
            memcpy(s_rx_buf + s_rx_len, ev->data, (size_t)ev->data_len);
            s_rx_len += (size_t)ev->data_len;
            if (s_rx_len == s_rx_total) {
                s_rx_buf[s_rx_len] = '\0';
                w.kind = WORK_MESSAGE;
                w.u.msg.topic = strdup(s_rx_topic);
                w.u.msg.payload = s_rx_buf;
                w.u.msg.len = s_rx_len;
                s_rx_buf = NULL;
                if (w.u.msg.topic == NULL || post_work(&w, 0) != ESP_OK) {
                    ESP_LOGW(TAG, "work queue full, dropped a message on %s", s_rx_topic);
                    work_free(&w);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "transport error (type %d)", (int)ev->error_handle->error_type);
            break;
        default:
            break;
    }
}

static bool log_rate_allow(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (s_log_tokens >= LOG_BURST) {
        s_log_refill_ms = now_ms;
    } else {
        int64_t gained = (now_ms - s_log_refill_ms) / LOG_REFILL_MS;
        if (gained > 0) {
            s_log_tokens += (int)gained;
            if (s_log_tokens > LOG_BURST) {
                s_log_tokens = LOG_BURST;
            }
            s_log_refill_ms = now_ms;
        }
    }

    if (s_log_tokens <= 0) {
        s_log_dropped++;
        return false;
    }
    s_log_tokens--;
    if (s_log_dropped > 0) {
        ESP_LOGI(TAG, "%u log events were not published (rate limit)", (unsigned)s_log_dropped);
        s_log_dropped = 0;
    }
    return true;
}

/** Runs in the default event loop task: it must hand over and return, never publish. */
static void gw_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (!s_connected) {
        return;
    }
    work_t w = {0};

    switch ((gw_event_id_t)id) {
        case GW_EVENT_BUS_STATE:
            w.kind = WORK_BUS;
            break;
        case GW_EVENT_GEAR_CHANGED:
            w.kind = WORK_GEAR;
            w.u.gear = *(const gw_event_gear_t *)data;
            break;
        case GW_EVENT_RESULT:
            w.kind = WORK_RESULT;
            w.u.result = *(const gw_event_result_t *)data;
            break;
        case GW_EVENT_PROGRESS:
            w.kind = WORK_PROGRESS;
            w.u.progress = *(const gw_event_progress_t *)data;
            break;
        case GW_EVENT_RX:
            /* Bus traffic can be dense; the work queue dropping one is preferable to stalling the
             * driver's listener task, which is what feeds it. */
            w.kind = WORK_RX;
            w.u.rx = *(const gw_event_rx_t *)data;
            break;
        case GW_EVENT_LOG:
            if (!log_rate_allow()) {
                return;
            }
            w.kind = WORK_LOG;
            w.u.log = *(const gw_event_log_t *)data;
            break;
        default:
            return;
    }
    post_work(&w, 0);
}

/* --- public API ------------------------------------------------------------------------------ */

static esp_err_t worker_start(void)
{
    if (s_queue == NULL) {
        s_queue = xQueueCreate(WORK_QUEUE_DEPTH, sizeof(work_t));
        ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "work queue");
    }
    if (s_worker == NULL) {
        BaseType_t ok =
            xTaskCreate(worker_task, "mqtt_iface", WORKER_STACK, NULL, WORKER_PRIO, &s_worker);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "worker task");
    }
    if (!s_events_bound) {
        ESP_RETURN_ON_ERROR(
            esp_event_handler_register(DALI_GW_EVENT, ESP_EVENT_ANY_ID, gw_event_handler, NULL),
            TAG, "event handler");
        s_events_bound = true;
    }
    return ESP_OK;
}

esp_err_t mqtt_iface_init(void)
{
    const app_config_t *cfg = app_config_get();

    if (!cfg->mqtt.enabled) {
        ESP_LOGI(TAG, "disabled in the configuration, not starting");
        return ESP_OK;
    }
    if (cfg->mqtt.uri[0] == '\0') {
        ESP_LOGW(TAG, "enabled but mqtt.uri is empty, not starting");
        return ESP_OK;
    }

    if (s_lwt_msg[0] == '\0') {
        cJSON *lwt = cJSON_CreateObject();
        ESP_RETURN_ON_FALSE(lwt != NULL, ESP_ERR_NO_MEM, TAG, "lwt");
        cJSON_AddStringToObject(lwt, "state", "offline");
        char *text = cJSON_PrintUnformatted(lwt);
        cJSON_Delete(lwt);
        ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_NO_MEM, TAG, "lwt");
        strlcpy(s_lwt_msg, text, sizeof(s_lwt_msg));
        cJSON_free(text);
    }

    ESP_RETURN_ON_ERROR(worker_start(), TAG, "worker");

    work_t w = {.kind = WORK_START};
    return post_work(&w, pdMS_TO_TICKS(200));
}

esp_err_t mqtt_iface_restart(void)
{
    if (s_queue == NULL) {
        return mqtt_iface_init();
    }
    const app_config_t *cfg = app_config_get();
    work_t w = {.kind = (cfg->mqtt.enabled && cfg->mqtt.uri[0] != '\0') ? WORK_RESTART : WORK_STOP};
    return post_work(&w, pdMS_TO_TICKS(200));
}

esp_err_t mqtt_iface_stop(void)
{
    if (s_queue == NULL) {
        return ESP_OK;
    }
    work_t w = {.kind = WORK_STOP};
    return post_work(&w, pdMS_TO_TICKS(200));
}

bool mqtt_iface_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_iface_publish_all(void)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    work_t w = {.kind = WORK_PUBLISH_ALL};
    return post_work(&w, 0);
}

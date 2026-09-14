/**
 * @file gw_events.h
 * @brief The single esp_event base used for every cross-component notification (SPEC 4).
 *
 * Payloads are small PODs copied into the loop. Anything larger — the gear list above all — is
 * pulled from the dali_bus registry by the consumer instead of being posted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_event.h"
#include "gw_api.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(DALI_GW_EVENT);

typedef enum {
    /** Network boot state changed. Payload: gw_event_net_t. */
    GW_EVENT_NET_STATE = 0,
    /** MQTT client connected or disconnected. Payload: gw_event_mqtt_t. */
    GW_EVENT_MQTT_STATE,
    /** A command finished. Payload: gw_event_result_t. */
    GW_EVENT_RESULT,
    /** A long operation advanced. Payload: gw_event_progress_t. */
    GW_EVENT_PROGRESS,
    /** One gear's cached state changed. Payload: gw_event_gear_t. */
    GW_EVENT_GEAR_CHANGED,
    /** Bus powered/busy state changed. Payload: gw_event_bus_t. */
    GW_EVENT_BUS_STATE,
    /** One DALI transaction happened, for the LED flicker. No payload. */
    GW_EVENT_BUS_ACTIVITY,
    /** A WARN+ line was logged. Payload: gw_event_log_t. */
    GW_EVENT_LOG,
    /** OTA started, progressed or finished. Payload: gw_event_ota_t. */
    GW_EVENT_OTA,
    /** The factory-reset button is being held. Payload: gw_event_factory_reset_t. */
    GW_EVENT_FACTORY_RESET_HOLD,
} gw_event_id_t;

/** Mirrors the boot state machine of SPEC 5.1; duplicated here so status_led need not know
 * net_wifi. */
typedef enum {
    GW_NET_BOOT = 0,
    GW_NET_AP_PROVISIONING,
    GW_NET_STA_CONNECTING,
    GW_NET_STA_CONNECTED,
    GW_NET_STA_FALLBACK_AP,
} gw_net_state_t;

typedef struct {
    gw_net_state_t state;
    char ip[16];
    int8_t rssi;
} gw_event_net_t;

typedef struct {
    bool connected;
} gw_event_mqtt_t;

typedef struct {
    uint32_t id;
    char action[GW_CMDNAME_LEN];
    bool ok;
    gw_err_t error;
    gw_target_t target;
    bool has_target;
    uint32_t duration_ms;
} gw_event_result_t;

typedef struct {
    char operation[GW_CMDNAME_LEN];
    uint16_t done, total, found;
} gw_event_progress_t;

typedef struct {
    uint8_t addr;
} gw_event_gear_t;

typedef struct {
    bool powered;
    bool busy;
} gw_event_bus_t;

typedef struct {
    char level[8]; /**< "warn" or "error" */
    char msg[128];
} gw_event_log_t;

typedef struct {
    bool in_progress;
    uint8_t percent;
    bool ok; /**< meaningful when in_progress is false */
} gw_event_ota_t;

typedef struct {
    uint16_t held_ms;
    bool triggered;
} gw_event_factory_reset_t;

/** @brief Post a POD payload on the default loop. Never blocks: a full loop drops the event. */
esp_err_t gw_event_post(gw_event_id_t id, const void *payload, size_t size);

#ifdef __cplusplus
}
#endif

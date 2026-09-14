/**
 * @file mqtt_iface.h
 * @brief esp-mqtt adapter: topic routing, LWT, retained state, HA discovery (SPEC 8).
 *
 * A thin adapter. Payloads are produced and consumed by gw_api; commands go to dali_bus through its
 * queue. Broker callbacks enqueue and return — they never block and never touch the bus.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the client if mqtt.enabled. Safe to call when disabled: it becomes a no-op. */
esp_err_t mqtt_iface_init(void);

/** @brief Apply a changed mqtt.* section by restarting the client in place (SPEC 6). */
esp_err_t mqtt_iface_restart(void);

esp_err_t mqtt_iface_stop(void);

bool mqtt_iface_connected(void);

/** @brief Republish the retained status, bus, gears and per-gear state topics. */
esp_err_t mqtt_iface_publish_all(void);

#ifdef __cplusplus
}
#endif

/**
 * @file dali_bus.h
 * @brief The only owner of the DALI bus (SPEC 7, ADR 0001).
 *
 * One FreeRTOS task drains a command queue and is the sole caller of the espressif/dali driver,
 * which is blocking and not thread-safe. Adapters submit commands and consume DALI_GW_EVENT; they
 * never touch the driver and never block waiting for the bus beyond an explicit timeout.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "gw_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DALI_BUS_QUEUE_DEPTH 32 /**< A full queue means bus_busy, never a blocked adapter. */

typedef struct {
    int8_t tx_gpio;
    int8_t rx_gpio;
    bool invert_tx;
    bool invert_rx;
    uint16_t poll_interval_s;
    bool scan_on_boot;
    uint16_t identify_blink_ms;
} dali_bus_config_t;

/** @brief Create the queue, the registry and the bus task. Does not touch the bus yet. */
esp_err_t dali_bus_init(const dali_bus_config_t *cfg);

/**
 * @brief Enqueue a command for asynchronous execution.
 *
 * Returns immediately. The outcome arrives as GW_EVENT_RESULT and, for the adapters that publish
 * it, on result/<action>.
 *
 * @retval ESP_ERR_NO_MEM  queue full; the caller must report GW_ERR_BUS_BUSY
 */
esp_err_t dali_bus_submit(const gw_cmd_t *cmd);

/**
 * @brief Enqueue a command and wait for its result.
 *
 * For HTTP handlers, which answer in-band (SPEC 9: 2 s timeout on the synchronous endpoints).
 * Must never be called from the bus task itself.
 *
 * @param[out] res  filled on success; release with gw_api_result_free()
 */
esp_err_t dali_bus_submit_sync(const gw_cmd_t *cmd, gw_result_t *res, uint32_t timeout_ms);

/** @brief Ask the running long operation to stop at its next step. */
esp_err_t dali_bus_cancel(void);

void dali_bus_get_status(gw_bus_status_t *out);

/**
 * @brief Copy one registry entry.
 *
 * @retval ESP_ERR_INVALID_ARG  addr > 63
 */
esp_err_t dali_bus_get_gear(uint8_t addr, gw_gear_t *out);

/**
 * @brief Copy every present gear, ordered by short address.
 *
 * @param[out] count  number of entries written
 */
esp_err_t dali_bus_get_gears(gw_gear_t *out, size_t max, size_t *count);

/** @brief Update the cached friendly name after app_config persisted it. */
esp_err_t dali_bus_set_gear_name(uint8_t addr, const char *name);

/* --- name tables ----------------------------------------------------------------------------
 * DALI opcode knowledge lives here, not in gw_api: the wire schema carries names as strings and
 * this component resolves them.
 */

/** @brief Resolve an indirect command name ("step_up"). @retval false unknown name. */
bool dali_bus_indirect_name_valid(const char *name);

/** @brief Resolve a query name ("actual_level") to its opcode. @retval -1 unknown name. */
int16_t dali_bus_query_opcode(const char *name);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Calaos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dali_system_components.h
 * @brief DALI Part 101 — Physical Layer & RMT Driver Core.
 *
 * Provides the low-level RMT-backed driver: hardware initialisation,
 * forward-frame transmission, backward-frame reception, and the raw
 * three-byte Part-103 transaction helper used by upper layers.
 *
 * All higher-level modules (dali_control_gear, dali_control_device, …) depend on this
 * header and must never access the dali_master_t internals directly.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "soc/gpio_num.h"
#include "dali_command.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Miscellaneous constants
 * ========================================================================= */

/** Default TX transmission timeout in milliseconds. */
#define DALI_TX_TIMEOUT_MS 110

/**
 * @brief Sentinel value returned in *result when no backward frame was received.
 */
#define DALI_RESULT_NO_REPLY (-1)

/**
 * @brief Test whether a query result contains a valid backward-frame byte.
 */
#define DALI_RESULT_IS_VALID(r) ((r) >= 0)

/* =========================================================================
 * Types
 * ========================================================================= */

/**
 * @brief DALI address types.
 */
typedef enum {
    DALI_ADDR_SHORT = 0,     /*!< Short address (0–63)    */
    DALI_ADDR_GROUP = 1,     /*!< Group address (0–15)    */
    DALI_ADDR_BROADCAST = 2, /*!< Broadcast               */
    DALI_ADDR_SPECIAL = 3,   /*!< Special command byte    */
} dali_addr_type_t;

/**
 * @brief Commissioning mode selector (used by both Part 102 and Part 103).
 */
typedef enum {
    DALI_COMMISSION_ALL =
        0, /*!< All gear/devices (Part 102: init byte = 0x00; Part 103: init byte = 0xFF) */
    DALI_COMMISSION_UNADDRESSED = 1, /*!< Unaddressed gear/devices only (Part 102: init byte = 0xFF;
                                        Part 103: init byte = 0x00) */
} dali_commission_mode_t;

/**
 * @brief Opaque handle for a DALI driver instance.
 */
typedef struct dali_master_t *dali_master_handle_t;

/**
 * @brief Bus-level configuration for a DALI master instance.
 */
typedef struct {
    gpio_num_t rx_gpio; /*!< GPIO for DALI bus RX */
    gpio_num_t tx_gpio; /*!< GPIO for DALI bus TX */
    bool invert_tx;     /*!< Invert TX signal polarity */
    bool invert_rx;     /*!< Invert RX signal polarity */
} dali_master_config_t;

/**
 * @brief RMT-backend specific configuration.
 */
typedef struct {
    uint32_t mem_block_symbols; /*!< 0 = auto-detect from SOC */
} dali_master_rmt_config_t;

/**
 * @brief Transaction configuration for dali_master_do_transaction().
 */
typedef struct {
    dali_addr_type_t addr_type;
    uint8_t addr;
    bool is_cmd;
    uint8_t command;
    bool send_twice;
    int tx_timeout_ms;
} dali_master_transaction_config_t;

/* =========================================================================
 * API — driver lifecycle
 * ========================================================================= */

/**
 * @brief Create and initialise a DALI master backed by RMT.
 */
esp_err_t dali_new_master_rmt(const dali_master_config_t *config,
                              const dali_master_rmt_config_t *rmt_config,
                              dali_master_handle_t *handle);

/**
 * @brief De-initialise and free a DALI master instance.
 */
esp_err_t dali_del_master(dali_master_handle_t handle);

/* =========================================================================
 * API — transaction
 * ========================================================================= */

/**
 * @brief Execute a 2-byte DALI forward frame and optionally receive a
 *        backward frame.
 */
esp_err_t dali_master_do_transaction(dali_master_handle_t handle,
                                     const dali_master_transaction_config_t *config, int *result);

/* =========================================================================
 * API — passive listening (local addition, see VENDORED.md)
 * ========================================================================= */

/**
 * @brief One frame observed on the bus.
 */
typedef struct {
    uint32_t frame;       /*!< Right-aligned frame bits, first bit on the wire is the MSB */
    uint8_t bits;         /*!< 8 (backward), 16 or 24 (forward) */
    int64_t timestamp_us; /*!< esp_timer time at which the RMT completed the capture, i.e. about
                               one idle-detection window after the frame's last edge */
} dali_rx_frame_t;

/**
 * @brief Delivered from the listener task, never from an ISR.
 *
 * Runs on the listener task with no lock held, so it may call back into any DALI API except
 * dali_master_listen_stop() and dali_del_master() (both wait for this task). Keep it short: the
 * RX channel is not re-armed until it returns.
 */
typedef void (*dali_rx_cb_t)(const dali_rx_frame_t *frame, void *user_data);

/**
 * @brief Start passive listening.
 *
 * Arms the RX channel whenever no transaction is in flight and reports every decoded frame to
 * @p cb. Transactions automatically suspend and resume listening; frames the master itself sent
 * are never reported.
 *
 * @param[in] handle    Handle from dali_new_master_rmt().
 * @param[in] cb        Frame callback. Must not be NULL.
 * @param[in] user_data Passed back to @p cb unchanged.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE if already listening,
 *         ESP_ERR_NO_MEM if the listener task could not be created, or ESP_ERR_TIMEOUT if a
 *         transaction did not release the RX channel in time.
 */
esp_err_t dali_master_listen_start(dali_master_handle_t handle, dali_rx_cb_t cb, void *user_data);

/**
 * @brief Stop passive listening and disarm the RX channel.
 *
 * Waits for a callback that is already running to finish, so @p user_data may be freed once this
 * returns ESP_OK. Returns ESP_ERR_TIMEOUT instead if the callback overran, in which case it may
 * still be running. Must not be called from the callback itself.
 */
esp_err_t dali_master_listen_stop(dali_master_handle_t handle);

/**
 * @brief Whether listening is currently requested (it is transparently suspended during a
 *        transaction, which does not change this).
 */
bool dali_master_listen_active(dali_master_handle_t handle);

/**
 * @brief True when the bus has been idle long enough that a forward frame may be sent.
 *
 * Combines an instantaneous read of the RX line with the time since the last activity this
 * driver knows about. Without listening running, the only activity it knows about is its own
 * transmissions, so a foreign master's frame is invisible to it beyond the line-level check.
 */
bool dali_master_bus_idle(dali_master_handle_t handle);

/* =========================================================================
 * API — raw frames
 * ========================================================================= */

/**
 * @brief Send a raw N-byte DALI frame (low-level physical layer API).
 *
 * This is the underlying frame transmission primitive used by both:
 * - dali_master_do_transaction() for standard 2-byte Part 102 frames
 * - Part 103 input device functions for 3-byte extended frames
 *
 * Most applications should use the higher-level APIs (dali_master_do_transaction
 * or dali_103_do_device_command) instead of calling this directly.
 *
 * @param[in]  handle        Handle from dali_new_master_rmt().
 * @param[in]  tx_buf        Raw bytes to send (Manchester-encoded by RMT).
 * @param[in]  tx_len        Number of bytes in tx_buf (2 for Part 102, 3 for Part 103).
 * @param[in]  send_twice    If true, sends the frame twice within 100 ms.
 * @param[in]  tx_timeout_ms TX timeout per frame (ms).
 * @param[out] result        Received backward frame byte, or DALI_RESULT_NO_REPLY.
 */
esp_err_t dali_master_do_raw_transaction(dali_master_handle_t handle, const uint8_t *tx_buf,
                                         size_t tx_len, bool send_twice, int tx_timeout_ms,
                                         int *result);

#ifdef __cplusplus
}
#endif

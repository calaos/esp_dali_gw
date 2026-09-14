/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Calaos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dali_frame_decode.h
 * @brief Manchester decoder for DALI frames captured by the RMT RX channel.
 *
 * Local addition to the vendored component (see VENDORED.md, patch 2). Kept free of
 * ESP-IDF dependencies apart from the RMT symbol layout so that it can be exercised by the
 * host test in `test/` without a toolchain.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef DALI_DECODE_HOST_TEST
#include "rmt_symbol_shim.h"
#else
#include "hal/rmt_types.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Longest run of one level the expander will emit, in Te. A run longer than 2 Te is already a
 *  framing violation or idle, so clamping loses no information and bounds the leading-idle run. */
#define DALI_TE_RUN_MAX 8

/** Te slots the expander can hold: 24-bit frame = 1 start + 24 data = 50 Te, + 4 Te stop. */
#define DALI_TE_MAX 72

typedef enum {
    DALI_DECODE_OK = 0,     /*!< 8, 16 or 24 data bits recovered */
    DALI_DECODE_NO_START,   /*!< nothing but idle, or no valid start bit */
    DALI_DECODE_TRUNCATED,  /*!< capture does not end in idle — buffer or memblock overflow */
    DALI_DECODE_BAD_LENGTH, /*!< Manchester-clean but not 8/16/24 bits */
} dali_decode_status_t;

/**
 * @brief Decode one captured RMT symbol stream into a DALI frame.
 *
 * Polarity is derived from the capture itself (the level of the trailing idle run), so the
 * result is the same whether the receiver inverts or not.
 *
 * @param[in]  symbols      Symbols as handed over by `rmt_rx_done_event_data_t`.
 * @param[in]  num_symbols  Number of valid entries in @p symbols.
 * @param[out] out_frame    Right-aligned frame bits, MSB first on the wire.
 * @param[out] out_bits     Number of data bits recovered (8, 16 or 24 on success).
 */
dali_decode_status_t dali_frame_decode(const rmt_symbol_word_t *symbols, size_t num_symbols,
                                       uint32_t *out_frame, uint8_t *out_bits);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Calaos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file rmt_symbol_shim.h
 * @brief Stand-in for `hal/rmt_types.h` so the decoder compiles on the host.
 *
 * Layout copied from ESP-IDF v6.0.2 `components/esp_hal_rmt/include/hal/rmt_types.h`; if that ever
 * changes the host test stops matching what runs on the device.
 */

#pragma once

#include <stdint.h>

typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;

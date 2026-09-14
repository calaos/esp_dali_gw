/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Calaos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dali_frame_decode.c
 * @brief Manchester decoder for DALI forward and backward frames.
 */

#include <stdbool.h>
#include "dali_frame_decode.h"

/** RMT resolution is 1 MHz, so one tick is one microsecond and Te is 416 ticks. */
#define DALI_TE_TICKS 416U

/** Shortest trailing idle run accepted as "the capture ended cleanly". The two stop bits are
 *  4 Te and the RMT only reports the frame after its idle threshold on top of that, so a real
 *  frame always exceeds this; anything shorter means the buffer filled up mid-frame. */
#define DALI_STOP_TE 3U

static size_t te_expand_run(uint8_t *te_levels, size_t te_len, uint8_t level, uint32_t ticks)
{
    uint32_t n = (ticks + (DALI_TE_TICKS / 2)) / DALI_TE_TICKS;
    if (n == 0) {
        n = 1;
    } else if (n > DALI_TE_RUN_MAX) {
        n = DALI_TE_RUN_MAX;
    }
    for (uint32_t k = 0; k < n && te_len < DALI_TE_MAX; k++) {
        te_levels[te_len++] = level;
    }
    return te_len;
}

dali_decode_status_t dali_frame_decode(const rmt_symbol_word_t *symbols, size_t num_symbols,
                                       uint32_t *out_frame, uint8_t *out_bits)
{
    *out_frame = 0;
    *out_bits = 0;

    if (symbols == NULL || num_symbols == 0) {
        return DALI_DECODE_NO_START;
    }

    /* Expand the run-length encoded capture into Te slots. A zero duration is the RMT's
     * end-of-stream marker, not a one-tick pulse: expanding it would fabricate a level. */
    uint8_t te_levels[DALI_TE_MAX];
    size_t te_len = 0;
    bool ended = false;

    for (size_t i = 0; i < num_symbols && te_len < DALI_TE_MAX && !ended; i++) {
        if (symbols[i].duration0 == 0) {
            ended = true;
            break;
        }
        te_len = te_expand_run(te_levels, te_len, (uint8_t)(symbols[i].level0 & 0x01U),
                               symbols[i].duration0);
        if (symbols[i].duration1 == 0) {
            ended = true;
            break;
        }
        te_len = te_expand_run(te_levels, te_len, (uint8_t)(symbols[i].level1 & 0x01U),
                               symbols[i].duration1);
    }

    if (te_len < 3) {
        return DALI_DECODE_NO_START;
    }

    /* The capture always ends in idle, so the trailing run gives the idle level without any
     * assumption about receiver polarity. If that run is too short the frame outgrew the
     * buffer and the tail we would decode is not the real end of the frame. */
    const uint8_t idle = te_levels[te_len - 1];
    size_t tail = 0;
    while (tail < te_len && te_levels[te_len - 1 - tail] == idle) {
        tail++;
    }
    if (tail < DALI_STOP_TE) {
        return DALI_DECODE_TRUNCATED;
    }

    /* Skip leading idle. Whether the RMT records it at all depends on how the capture was
     * armed relative to the first edge, so handle both. */
    size_t i = 0;
    while (i < te_len && te_levels[i] == idle) {
        i++;
    }

    /* Start bit is a logic 1: asserted for 1 Te, then idle for 1 Te. That fixes the half-bit
     * pattern of a '1' for the rest of the frame. */
    if (i + 1 >= te_len || te_levels[i] == idle || te_levels[i + 1] != idle) {
        return DALI_DECODE_NO_START;
    }
    const uint8_t one_a = te_levels[i];
    const uint8_t one_b = te_levels[i + 1];
    i += 2;

    uint32_t frame = 0;
    uint8_t bits = 0;

    while (bits < 24 && (i + 1) < te_len) {
        const uint8_t a = te_levels[i];
        const uint8_t b = te_levels[i + 1];
        if (a == one_a && b == one_b) {
            frame = (frame << 1) | 1U;
        } else if (a == one_b && b == one_a) {
            frame = frame << 1;
        } else {
            break; /* stop bits, or a violation */
        }
        bits++;
        i += 2;
    }

    if (bits != 8 && bits != 16 && bits != 24) {
        return DALI_DECODE_BAD_LENGTH;
    }

    /* Everything after the last data bit must be idle, otherwise the "stop" we stopped on was
     * a corrupted half-bit and the bit count we happen to have landed on is meaningless. */
    for (size_t k = i; k < te_len; k++) {
        if (te_levels[k] != idle) {
            return DALI_DECODE_BAD_LENGTH;
        }
    }

    *out_frame = frame;
    *out_bits = bits;
    return DALI_DECODE_OK;
}

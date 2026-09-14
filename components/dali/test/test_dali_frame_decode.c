/*
 * SPDX-FileCopyrightText: 2026 Calaos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file test_dali_frame_decode.c
 * @brief Host test for the DALI Manchester decoder. See components/dali/test/README.md.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dali_frame_decode.h"

#define TE_US 416

static int s_fail;
static int s_run;

#define CHECK(cond, fmt, ...)                                                                      \
    do {                                                                                           \
        s_run++;                                                                                   \
        if (!(cond)) {                                                                             \
            s_fail++;                                                                              \
            printf("FAIL %s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);                     \
        }                                                                                          \
    } while (0)

/* ---------------------------------------------------------------------------
 * Frame generator: bits -> Te levels -> run-length encoded RMT symbols
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t lvl[256];
    size_t len;
} te_seq_t;

static void te_push(te_seq_t *s, uint8_t level, size_t count)
{
    while (count--) {
        s->lvl[s->len++] = level;
    }
}

/**
 * @param idle        Level the line rests at (1 for a non-inverting receiver).
 * @param lead_idle   Te of recorded idle before the start bit (0 = capture starts on the edge).
 * @param trail_idle  Te of idle after the stop bits (>= the RMT idle threshold in practice).
 */
static void gen_frame(te_seq_t *s, uint32_t frame, uint8_t bits, uint8_t idle, size_t lead_idle,
                      size_t trail_idle)
{
    const uint8_t asserted = (uint8_t)(idle ^ 1U);
    s->len = 0;
    te_push(s, idle, lead_idle);
    te_push(s, asserted, 1); /* start bit = logic 1 */
    te_push(s, idle, 1);
    for (int i = bits - 1; i >= 0; i--) {
        if ((frame >> i) & 1U) {
            te_push(s, asserted, 1);
            te_push(s, idle, 1);
        } else {
            te_push(s, idle, 1);
            te_push(s, asserted, 1);
        }
    }
    te_push(s, idle, 4); /* two stop bits */
    te_push(s, idle, trail_idle);
}

/** Run-length encode a Te sequence into RMT symbols, with optional per-run timing error. */
static size_t te_to_symbols(const te_seq_t *s, rmt_symbol_word_t *out, size_t out_max,
                            int jitter_pct)
{
    uint32_t runs[256];
    uint8_t levels[256];
    size_t nruns = 0;

    for (size_t i = 0; i < s->len;) {
        size_t j = i;
        while (j < s->len && s->lvl[j] == s->lvl[i]) {
            j++;
        }
        levels[nruns] = s->lvl[i];
        uint32_t ticks = (uint32_t)((j - i) * TE_US);
        if (jitter_pct) {
            int sign = (int)(nruns & 1U) ? 1 : -1;
            ticks = (uint32_t)((int)ticks + sign * (int)ticks * jitter_pct / 100);
        }
        runs[nruns++] = ticks;
        i = j;
    }

    size_t n = 0;
    for (size_t i = 0; i < nruns && n < out_max; i += 2) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].level0 = levels[i] & 1U;
        out[n].duration0 = (uint16_t)(runs[i] > 0x7FFF ? 0x7FFF : runs[i]);
        if (i + 1 < nruns) {
            out[n].level1 = levels[i + 1] & 1U;
            out[n].duration1 = (uint16_t)(runs[i + 1] > 0x7FFF ? 0x7FFF : runs[i + 1]);
        }
        n++;
    }
    return n;
}

static dali_decode_status_t decode_frame(uint32_t frame, uint8_t bits, uint8_t idle, size_t lead,
                                         size_t trail, int jitter_pct, uint32_t *out,
                                         uint8_t *obits)
{
    te_seq_t seq;
    rmt_symbol_word_t syms[128];
    gen_frame(&seq, frame, bits, idle, lead, trail);
    size_t n = te_to_symbols(&seq, syms, 128, jitter_pct);
    return dali_frame_decode(syms, n, out, obits);
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/** Every 16-bit forward frame, both polarities, with and without recorded leading idle. */
static void test_roundtrip_16bit(void)
{
    for (uint32_t f = 0; f <= 0xFFFF; f++) {
        for (uint8_t idle = 0; idle <= 1; idle++) {
            uint32_t got = 0;
            uint8_t gb = 0;
            dali_decode_status_t st = decode_frame(f, 16, idle, (f & 1U) ? 6 : 0, 8, 0, &got, &gb);
            if (st != DALI_DECODE_OK || got != f || gb != 16) {
                CHECK(0, "16-bit 0x%04X idle=%u -> st=%d got=0x%04X bits=%u", f, idle, st, got, gb);
                return;
            }
        }
    }
    CHECK(1, "");
}

/** Every 8-bit backward frame — the reply path the transaction code depends on. */
static void test_roundtrip_8bit(void)
{
    for (uint32_t f = 0; f <= 0xFF; f++) {
        uint32_t got = 0;
        uint8_t gb = 0;
        dali_decode_status_t st = decode_frame(f, 8, 1, 4, 8, 0, &got, &gb);
        CHECK(st == DALI_DECODE_OK && got == f && gb == 8,
              "8-bit 0x%02X -> st=%d got=0x%02X bits=%u", f, st, got, gb);
    }
}

/** 24-bit Part 103 frames: the length the vendored decoder exists to support. */
static void test_roundtrip_24bit(void)
{
    static const uint32_t vectors[] = {
        0x000000, 0xFFFFFF, 0xA5A5A5, 0x010203, 0xFE0180, 0x800001, 0x555555, 0xAAAAAA,
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        for (uint8_t idle = 0; idle <= 1; idle++) {
            uint32_t got = 0;
            uint8_t gb = 0;
            dali_decode_status_t st = decode_frame(vectors[i], 24, idle, 2, 8, 0, &got, &gb);
            CHECK(st == DALI_DECODE_OK && got == vectors[i] && gb == 24,
                  "24-bit 0x%06X idle=%u -> st=%d got=0x%06X bits=%u", vectors[i], idle, st, got,
                  gb);
        }
    }
}

/** IEC 62386 allows Te to deviate by +-10 %. */
static void test_timing_tolerance(void)
{
    for (int jit = -10; jit <= 10; jit += 5) {
        uint32_t got = 0;
        uint8_t gb = 0;
        dali_decode_status_t st = decode_frame(0xFF80, 16, 1, 4, 8, jit, &got, &gb);
        CHECK(st == DALI_DECODE_OK && got == 0xFF80 && gb == 16, "jitter %d%% -> st=%d got=0x%04X",
              jit, st, got);
    }
}

/** A capture that fills the buffer before the stop bits must not decode as a short frame. */
static void test_truncated_capture(void)
{
    te_seq_t seq;
    rmt_symbol_word_t syms[128];
    uint32_t got = 0;
    uint8_t gb = 0;

    gen_frame(&seq, 0xA5A5, 16, 1, 0, 8);
    size_t n = te_to_symbols(&seq, syms, 128, 0);
    CHECK(dali_frame_decode(syms, n - 3, &got, &gb) == DALI_DECODE_TRUNCATED,
          "truncated not caught");
}

/** The forward frame the master itself sent must never be mistaken for a reply. */
static void test_forward_frame_is_not_a_reply(void)
{
    uint32_t got = 0;
    uint8_t gb = 0;
    dali_decode_status_t st = decode_frame(0xFE00, 16, 1, 0, 8, 0, &got, &gb);
    CHECK(st == DALI_DECODE_OK && gb == 16, "FF echo decoded as %d bits (st=%d)", gb, st);
}

/** The RMT marks the end of a capture with a zero-duration symbol; that must not be read as
 *  a one-tick pulse at an arbitrary level. */
static void test_end_marker_symbol(void)
{
    te_seq_t seq;
    rmt_symbol_word_t syms[128];
    uint32_t got = 0;
    uint8_t gb = 0;

    gen_frame(&seq, 0xFE80, 16, 1, 0, 6);
    size_t n = te_to_symbols(&seq, syms, 127, 0);
    memset(&syms[n], 0, sizeof(syms[n]));
    syms[n].level0 = 0; /* a level that would corrupt the tail if the marker were expanded */
    n++;
    CHECK(dali_frame_decode(syms, n, &got, &gb) == DALI_DECODE_OK && got == 0xFE80 && gb == 16,
          "end marker mishandled -> 0x%04X (%u bits)", got, gb);
}

/** Lengths DALI does not define must be rejected rather than reported as a short frame. */
static void test_bad_lengths(void)
{
    const uint8_t lens[] = {1, 4, 7, 9, 12, 17, 20, 23};
    for (size_t i = 0; i < sizeof(lens); i++) {
        uint32_t got = 0;
        uint8_t gb = 0;
        dali_decode_status_t st = decode_frame(0x5, lens[i], 1, 2, 8, 0, &got, &gb);
        CHECK(st == DALI_DECODE_BAD_LENGTH, "%u data bits -> st=%d (expected BAD_LENGTH)", lens[i],
              st);
    }
}

/** Silence, a lone glitch, and a Manchester violation in the middle of a frame. */
static void test_garbage(void)
{
    uint32_t got = 0;
    uint8_t gb = 0;
    rmt_symbol_word_t syms[8];

    CHECK(dali_frame_decode(NULL, 0, &got, &gb) == DALI_DECODE_NO_START, "NULL accepted");

    memset(syms, 0, sizeof(syms));
    CHECK(dali_frame_decode(syms, 4, &got, &gb) == DALI_DECODE_NO_START, "zero-duration accepted");

    /* Idle only: the line never left its rest level. */
    memset(syms, 0, sizeof(syms));
    syms[0].level0 = 1;
    syms[0].duration0 = 20000;
    CHECK(dali_frame_decode(syms, 1, &got, &gb) == DALI_DECODE_NO_START, "idle-only accepted");

    /* Three Te at the asserted level cannot occur inside a frame. */
    te_seq_t seq;
    gen_frame(&seq, 0xA5A5, 16, 1, 0, 8);
    seq.lvl[10] = 0;
    seq.lvl[11] = 0;
    seq.lvl[12] = 0;
    rmt_symbol_word_t big[128];
    size_t n = te_to_symbols(&seq, big, 128, 0);
    dali_decode_status_t st = dali_frame_decode(big, n, &got, &gb);
    CHECK(st != DALI_DECODE_OK, "Manchester violation decoded as 0x%04X (%u bits)", got, gb);
}

int main(void)
{
    test_roundtrip_8bit();
    test_roundtrip_16bit();
    test_roundtrip_24bit();
    test_timing_tolerance();
    test_truncated_capture();
    test_end_marker_symbol();
    test_forward_frame_is_not_a_reply();
    test_bad_lengths();
    test_garbage();

    printf("%d checks, %d failures\n", s_run, s_fail);
    return s_fail ? 1 : 0;
}

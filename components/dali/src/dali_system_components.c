/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Calaos
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file dali_system_components.c
 * @brief DALI Part 101 — Physical Layer & RMT Driver Core.
 *
 * Implements the RMT-backed DALI master driver: hardware initialisation,
 * Manchester-encoded forward-frame TX, backward-frame RX/decode,
 * and the raw transaction dispatcher (dali_master_do_raw_transaction /
 * dali_master_do_transaction).
 *
 * ## Who owns the RX channel
 *
 * Two contexts want the single RMT RX channel: the listener task added here, and whichever task
 * runs a transaction. `listen_mux` is the token of ownership and is held for the *whole*
 * transaction, so the two can never have a receive armed at the same time.
 *
 *   transaction    take listen_mux -> (if owned) drop listen_owned/pending, rmt_disable(rx)
 *                                  -> enable, TX, optional BF window, disable, IFG
 *                                  -> post RESUME -> give listen_mux
 *   listener task  take listen_mux -> arm if wanted and not armed -> give listen_mux
 *                                  -> block on listen_q (no lock held)
 *                                  -> decode + user callback (no lock held)
 *
 * Two flags say what the listener holds: `listen_owned` (it called `rmt_enable` and owes the
 * matching disable) and `listen_pending` (a receive of its own is in flight, which is also what
 * the RX-done ISR uses to pick the right queue). Keeping them apart is what stops a transaction
 * from calling `rmt_enable` on an already-enabled channel after a capture has just finished.
 *
 * The one ordering that matters is clearing them *before* `rmt_disable()`. The RX-done
 * ISR is the only other writer of channel state and it cannot run concurrently on a single core,
 * so after that store the ISR can no longer decide to keep listening. `rmt_disable()` itself is
 * safe from either the enabled or the running state (IDF 6 `rmt_rx_disable()` accepts both and
 * cancels an in-flight receive with register writes only), which is why the transaction never has
 * to find out which of the two it is in, and never has to wait for a frame to finish.
 *
 * An event the ISR queued just before the handover describes a frame that really was on the bus,
 * so it is kept, not flushed; the listener picks it up after the transaction.
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "dali_frame_decode.h"
#include "dali_system_components.h"

static const char *TAG = "dali";

/* -------------------------------------------------------------------------
 * RMT timing constants
 * ------------------------------------------------------------------------- */

/** RMT clock resolution (1 MHz → 1 µs per tick). */
#define DALI_RMT_RESOLUTION_HZ 1000000U

/** Convert microseconds to RMT ticks. */
#define DALI_US_TO_RMT_TICKS(us) ((us) * (DALI_RMT_RESOLUTION_HZ / 1000000U))

/** Convert microseconds to nanoseconds. */
#define DALI_US_TO_NS(us) ((us) * 1000U)

/** DALI half-period Te in microseconds (nominal 416.67 µs). */
#define DALI_TE_US 416U

/** Delay between first and second TX of a send-twice command (ms). */
#define DALI_SEND_TWICE_DELAY_MS 40

/** Backward-frame receive window timeout (ms).
 *  IEC 62386: response window 7–22 Te + BF frame 22 Te = max ~18.3 ms; 25 ms gives margin. */
#define DALI_BF_TIMEOUT_MS 25

/** Minimum inter-frame gap inserted after every transaction (ms).
 *  IEC 62386 requires > 22 Te ≈ 9.2 ms; 20 ms gives comfortable margin. */
#define DALI_IFG_MS 20

/** Idle threshold that ends a listen capture. Must exceed the longest run inside a frame
 *  (2 Te + 10 % = 916 us) yet stay well below the 7 Te that a backward frame may follow the
 *  forward frame by, because the channel is only re-armed once the capture has been reported. */
#define DALI_LISTEN_IDLE_US 1400U

/** Symbols per listen capture. A 24-bit frame alternating every Te is 25 symbols; the rest is
 *  headroom for a noisy line. */
#define DALI_LISTEN_SYMBOLS 64

/** How long the listener waits for the RX channel before retrying. Only a transaction can hold
 *  it, and that is bounded by the TX timeout plus the BF window and the inter-frame gap. */
#define DALI_LISTEN_MUX_WAIT_MS 1000

/** Bus quiet time after which dali_master_bus_idle() reports the bus as free.
 *  IEC 62386 requires > 22 Te (about 9.2 ms) between frames. */
#define DALI_BUS_IDLE_US (DALI_IFG_MS * 1000)

/** Address mask bits for each addressing mode (S-bit excluded). */
#define DALI_ADDR_MASK_SHORT 0x00U
#define DALI_ADDR_MASK_GROUP 0x80U
#define DALI_ADDR_MASK_BROADCAST 0xFEU

/* -------------------------------------------------------------------------
 * Internal driver context
 * ------------------------------------------------------------------------- */

/** What woke the listener task. */
typedef enum {
    DALI_LISTEN_EVT_FRAME = 0, /*!< a capture completed, listen_raw holds it */
    DALI_LISTEN_EVT_WAKE,      /*!< start/stop/resume: re-evaluate the channel state */
} dali_listen_evt_kind_t;

typedef struct {
    dali_listen_evt_kind_t kind;
    size_t num_symbols;
    int64_t timestamp_us;
} dali_listen_evt_t;

struct dali_master_t {
    rmt_channel_handle_t rx_channel;
    rmt_channel_handle_t tx_channel;
    rmt_encoder_handle_t tx_encoder;
    QueueHandle_t rx_queue;
    rmt_receive_config_t rx_cfg;
    rmt_symbol_word_t rx_raw[32]; /* static DMA buffer — must not be on stack */

    /* TX symbol pointers — set once during init based on polarity config. */
    const rmt_symbol_word_t *symbol_one;
    const rmt_symbol_word_t *symbol_zero;
    const rmt_symbol_word_t *symbol_stop;

    /* --- listen mode (local addition, see VENDORED.md) --- */
    gpio_num_t rx_gpio;
    bool invert_rx;
    SemaphoreHandle_t listen_mux; /* ownership of the RX channel, see the file header */
    TaskHandle_t listen_task;
    QueueHandle_t listen_q;
    SemaphoreHandle_t listen_exited;
    SemaphoreHandle_t listen_idle; /* given each time the task is about to block */
    rmt_receive_config_t listen_cfg;
    rmt_symbol_word_t listen_raw[DALI_LISTEN_SYMBOLS];
    dali_rx_cb_t listen_cb;
    void *listen_user;
    bool listen_on;               /* user intent, guarded by listen_mux */
    volatile bool listen_owned;   /* the listener called rmt_enable() and owes the disable */
    volatile bool listen_pending; /* a listener receive is in flight; read from the ISR */
    volatile bool listen_quit;
    volatile int64_t last_activity_us;
};

/* -------------------------------------------------------------------------
 * Static TX symbol tables
 * ------------------------------------------------------------------------- */

/* --- Non-inverting (invert_tx = false, default) --- */
static const rmt_symbol_word_t s_sym_one_normal = {
    .level0 = 0,
    .duration0 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
    .level1 = 1,
    .duration1 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
};
static const rmt_symbol_word_t s_sym_zero_normal = {
    .level0 = 1,
    .duration0 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
    .level1 = 0,
    .duration1 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
};
static const rmt_symbol_word_t s_sym_stop_normal = {
    .level0 = 0,
    .duration0 = DALI_US_TO_RMT_TICKS(DALI_TE_US) * 2,
    .level1 = 0,
    .duration1 = DALI_US_TO_RMT_TICKS(DALI_TE_US) * 2,
};

/* --- Inverting (invert_tx = true) --- */
static const rmt_symbol_word_t s_sym_one_invert = {
    .level0 = 1,
    .duration0 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
    .level1 = 0,
    .duration1 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
};
static const rmt_symbol_word_t s_sym_zero_invert = {
    .level0 = 0,
    .duration0 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
    .level1 = 1,
    .duration1 = DALI_US_TO_RMT_TICKS(DALI_TE_US),
};
static const rmt_symbol_word_t s_sym_stop_invert = {
    .level0 = 0,
    .duration0 = DALI_US_TO_RMT_TICKS(DALI_TE_US) * 2,
    .level1 = 0,
    .duration1 = DALI_US_TO_RMT_TICKS(DALI_TE_US) * 2,
};

/* -------------------------------------------------------------------------
 * Private helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief RMT simple-encoder callback for DALI forward frames.
 */
static size_t dali_tx_encode_cb(const void *data, size_t data_size, size_t symbols_written,
                                size_t symbols_free, rmt_symbol_word_t *symbols, bool *done,
                                void *arg)
{
    struct dali_master_t *dev = (struct dali_master_t *)arg;

    if (symbols_free < 10) {
        return 0;
    }
    if (symbols_written == 0) {
        symbols[0] = *dev->symbol_one;
        return 1;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    size_t byte_idx = (symbols_written - 1) / 8;
    if (byte_idx < data_size) {
        size_t out = 0;
        for (int mask = 0x80; mask != 0; mask >>= 1) {
            symbols[out++] = (bytes[byte_idx] & mask) ? *dev->symbol_one : *dev->symbol_zero;
        }
        return out;
    }
    symbols[0] = *dev->symbol_stop;
    *done = true;
    return 1;
}

/**
 * @brief RX-done ISR: routes the capture to whoever armed it.
 *
 * Exactly one of the two paths can have a receive armed at a time (see the file header), so
 * listen_pending is an unambiguous selector rather than a guess.
 */
static bool IRAM_ATTR dali_rx_done_cb(rmt_channel_handle_t channel,
                                      const rmt_rx_done_event_data_t *edata, void *user_data)
{
    struct dali_master_t *dev = (struct dali_master_t *)user_data;
    BaseType_t hp_woken = pdFALSE;
    (void)channel;

    if (dev->listen_pending) {
        /* Only the symbol count travels: the symbols stay in listen_raw, which nothing re-arms
         * until the listener task has decoded them. */
        const dali_listen_evt_t evt = {
            .kind = DALI_LISTEN_EVT_FRAME,
            .num_symbols = edata->num_symbols,
            .timestamp_us = esp_timer_get_time(),
        };
        xQueueSendFromISR(dev->listen_q, &evt, &hp_woken);
    } else {
        xQueueSendFromISR(dev->rx_queue, edata, &hp_woken);
    }
    return hp_woken == pdTRUE;
}

/* -------------------------------------------------------------------------
 * Listen mode (local addition, see VENDORED.md)
 * ------------------------------------------------------------------------- */

/** Wake the listener task so it re-evaluates whether the RX channel should be armed. */
static void dali_listen_wake(struct dali_master_t *dev)
{
    const dali_listen_evt_t evt = {.kind = DALI_LISTEN_EVT_WAKE};
    xQueueSend(dev->listen_q, &evt, 0);
}

/** Bring the channel in line with listen_on. Callers must not hold listen_mux. */
static void dali_listen_sync(struct dali_master_t *dev)
{
    if (xSemaphoreTake(dev->listen_mux, pdMS_TO_TICKS(DALI_LISTEN_MUX_WAIT_MS)) != pdTRUE) {
        return; /* a transaction still owns the channel; the next event retries */
    }

    esp_err_t err = ESP_OK;
    if (dev->listen_on) {
        if (!dev->listen_owned) {
            err = rmt_enable(dev->rx_channel);
            if (err == ESP_OK) {
                dev->listen_owned = true;
            }
        }
        if (dev->listen_owned && !dev->listen_pending) {
            /* Claim the ISR before starting the hardware: a frame may complete between the two. */
            dev->listen_pending = true;
            err = rmt_receive(dev->rx_channel, dev->listen_raw, sizeof(dev->listen_raw),
                              &dev->listen_cfg);
            if (err != ESP_OK) {
                dev->listen_pending = false;
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "listen arm failed: %s", esp_err_to_name(err));
        }
    } else if (dev->listen_owned) {
        dev->listen_pending = false;
        dev->listen_owned = false;
        rmt_disable(dev->rx_channel);
    }

    xSemaphoreGive(dev->listen_mux);
}

static void dali_listen_task(void *arg)
{
    struct dali_master_t *dev = (struct dali_master_t *)arg;

    while (!dev->listen_quit) {
        dali_listen_sync(dev);

        /* Tells dali_master_listen_stop() that no callback is running right now. */
        xSemaphoreGive(dev->listen_idle);

        dali_listen_evt_t evt;
        if (xQueueReceive(dev->listen_q, &evt, pdMS_TO_TICKS(DALI_LISTEN_MUX_WAIT_MS)) != pdPASS) {
            continue; /* nothing on the bus; loop so a failed arm is retried */
        }
        if (evt.kind != DALI_LISTEN_EVT_FRAME) {
            continue;
        }

        /* The capture is consumed before the channel is re-armed, so listen_raw has a single
         * owner at all times and no second buffer is needed. This task is the only writer that
         * sets listen_pending (in dali_listen_sync); pause and stop can only clear it. */
        dev->listen_pending = false;
        dev->last_activity_us = evt.timestamp_us;

        uint32_t frame = 0;
        uint8_t bits = 0;
        dali_decode_status_t dec =
            dali_frame_decode(dev->listen_raw, evt.num_symbols, &frame, &bits);
        if (dec != DALI_DECODE_OK) {
            ESP_LOGD(TAG, "listen: dropped capture (status %d, symbols %u)", (int)dec,
                     (unsigned)evt.num_symbols);
            continue;
        }

        dali_rx_cb_t cb = dev->listen_cb;
        if (cb) {
            const dali_rx_frame_t out = {
                .frame = frame,
                .bits = bits,
                .timestamp_us = evt.timestamp_us,
            };
            cb(&out, dev->listen_user);
        }
    }

    xSemaphoreGive(dev->listen_exited);
    vTaskDelete(NULL);
}

/**
 * @brief Hand the RX channel over to a transaction.
 *
 * Takes listen_mux (held until dali_listen_resume()) and cancels any receive the listener has
 * armed. Clearing the ownership flags first is what makes this race-free; see the file header.
 */
static esp_err_t dali_listen_pause(struct dali_master_t *dev, int tx_timeout_ms)
{
    const TickType_t wait =
        pdMS_TO_TICKS(DALI_LISTEN_MUX_WAIT_MS + (tx_timeout_ms > 0 ? tx_timeout_ms : 0));
    if (xSemaphoreTake(dev->listen_mux, wait) != pdTRUE) {
        ESP_LOGE(TAG, "timed out taking the RX channel");
        return ESP_ERR_TIMEOUT;
    }
    if (dev->listen_owned) {
        dev->listen_pending = false;
        dev->listen_owned = false;
        rmt_disable(dev->rx_channel); /* valid from the running state, cancels the capture */
    }
    return ESP_OK;
}

static void dali_listen_resume(struct dali_master_t *dev)
{
    dev->last_activity_us = esp_timer_get_time();
    xSemaphoreGive(dev->listen_mux);
    if (dev->listen_on) {
        dali_listen_wake(dev);
    }
}

esp_err_t dali_master_listen_start(dali_master_handle_t handle, dali_rx_cb_t cb, void *user_data)
{
    ESP_RETURN_ON_FALSE(handle != NULL && cb != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "handle and cb must not be NULL");
    struct dali_master_t *dev = handle;

    ESP_RETURN_ON_FALSE(xSemaphoreTake(dev->listen_mux, pdMS_TO_TICKS(DALI_LISTEN_MUX_WAIT_MS)) ==
                            pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "timed out taking the RX channel");

    esp_err_t ret = ESP_OK;
    if (dev->listen_on) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }

    if (dev->listen_task == NULL &&
        xTaskCreate(dali_listen_task, "dali_listen", CONFIG_DALI_LISTEN_TASK_STACK, dev,
                    CONFIG_DALI_LISTEN_TASK_PRIORITY, &dev->listen_task) != pdPASS) {
        dev->listen_task = NULL;
        ret = ESP_ERR_NO_MEM;
        goto out;
    }

    dev->listen_cb = cb;
    dev->listen_user = user_data;
    dev->listen_on = true;

out:
    xSemaphoreGive(dev->listen_mux);
    if (ret == ESP_OK) {
        dali_listen_wake(dev);
    }
    return ret;
}

esp_err_t dali_master_listen_stop(dali_master_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle must not be NULL");
    struct dali_master_t *dev = handle;

    ESP_RETURN_ON_FALSE(xSemaphoreTake(dev->listen_mux, pdMS_TO_TICKS(DALI_LISTEN_MUX_WAIT_MS)) ==
                            pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "timed out taking the RX channel");
    dev->listen_on = false;
    if (dev->listen_owned) {
        dev->listen_pending = false;
        dev->listen_owned = false;
        rmt_disable(dev->rx_channel);
    }
    dev->listen_cb = NULL;
    dev->listen_user = NULL;
    xSemaphoreGive(dev->listen_mux);

    /* Drain a capture the ISR queued during the handover so the next start does not report it. */
    dali_listen_evt_t drop;
    while (xQueueReceive(dev->listen_q, &drop, 0) == pdPASS) {
    }

    if (dev->listen_task == NULL) {
        return ESP_OK;
    }

    /* Wait for the task to come back round to its blocking point, so a callback that was already
     * running has finished and the caller may free whatever user_data pointed at. Discard any
     * stale token first, otherwise we would accept a lap that started before the stop. */
    (void)xSemaphoreTake(dev->listen_idle, 0);
    dali_listen_wake(dev);
    if (xSemaphoreTake(dev->listen_idle, pdMS_TO_TICKS(DALI_LISTEN_MUX_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "listener still busy; a frame callback may still be running");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool dali_master_listen_active(dali_master_handle_t handle)
{
    return handle != NULL && handle->listen_on;
}

bool dali_master_bus_idle(dali_master_handle_t handle)
{
    if (handle == NULL) {
        return false;
    }
    struct dali_master_t *dev = handle;

    /* invert_rx is applied inside the RMT, so the raw pin rests high unless the receiver
     * inverts. A line that is asserted right now is conclusive; the timestamp covers the rest. */
    const int idle_level = dev->invert_rx ? 0 : 1;
    if (gpio_get_level(dev->rx_gpio) != idle_level) {
        return false;
    }
    return (esp_timer_get_time() - dev->last_activity_us) >= DALI_BUS_IDLE_US;
}

/* -------------------------------------------------------------------------
 * Public API — driver lifecycle
 * ------------------------------------------------------------------------- */

esp_err_t dali_new_master_rmt(const dali_master_config_t *config,
                              const dali_master_rmt_config_t *rmt_config,
                              dali_master_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "config and handle must not be NULL");
    ESP_RETURN_ON_FALSE(config->rx_gpio >= 0 && config->tx_gpio >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid GPIO number: rx=%d tx=%d", config->rx_gpio, config->tx_gpio);

    const dali_master_rmt_config_t default_rmt_cfg = {
        .mem_block_symbols = 64,
    };
    if (rmt_config == NULL) {
        rmt_config = &default_rmt_cfg;
    }

    ESP_LOGI(TAG, "TX polarity: %s, RX polarity: %s", config->invert_tx ? "inverting" : "normal",
             config->invert_rx ? "inverting" : "normal");

    uint32_t mem_block = rmt_config->mem_block_symbols;
    if (mem_block == 0) {
#if defined(SOC_RMT_MEM_WORDS_PER_CHANNEL)
        mem_block = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
        mem_block = 64;
#endif
    }

    esp_err_t ret = ESP_OK;

    struct dali_master_t *dev = calloc(1, sizeof(struct dali_master_t));
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate DALI context");

    if (config->invert_tx) {
        dev->symbol_one = &s_sym_one_invert;
        dev->symbol_zero = &s_sym_zero_invert;
        dev->symbol_stop = &s_sym_stop_invert;
    } else {
        dev->symbol_one = &s_sym_one_normal;
        dev->symbol_zero = &s_sym_zero_normal;
        dev->symbol_stop = &s_sym_stop_normal;
    }

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DALI_RMT_RESOLUTION_HZ,
        .mem_block_symbols = mem_block,
        .gpio_num = config->rx_gpio,
        .flags.invert_in = config->invert_rx ? 1 : 0,
    };
    ESP_GOTO_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &dev->rx_channel), err, TAG,
                      "Failed to create RX channel");

    dev->rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    ESP_GOTO_ON_FALSE(dev->rx_queue != NULL, ESP_ERR_NO_MEM, err, TAG, "Failed to create RX queue");

    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = dali_rx_done_cb,
    };
    ESP_GOTO_ON_ERROR(rmt_rx_register_event_callbacks(dev->rx_channel, &rx_cbs, dev), err, TAG,
                      "Failed to register RX callbacks");

    dev->rx_cfg = (rmt_receive_config_t){
        .signal_range_min_ns = DALI_US_TO_NS(2),
        .signal_range_max_ns = DALI_US_TO_NS(2000),
    };

    dev->rx_gpio = config->rx_gpio;
    dev->invert_rx = config->invert_rx;
    dev->listen_cfg = (rmt_receive_config_t){
        .signal_range_min_ns = DALI_US_TO_NS(2),
        .signal_range_max_ns = DALI_US_TO_NS(DALI_LISTEN_IDLE_US),
    };
    dev->listen_mux = xSemaphoreCreateMutex();
    dev->listen_q = xQueueCreate(4, sizeof(dali_listen_evt_t));
    dev->listen_exited = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(dev->listen_mux != NULL && dev->listen_q != NULL &&
                          dev->listen_exited != NULL,
                      ESP_ERR_NO_MEM, err, TAG, "Failed to create listen resources");

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = DALI_RMT_RESOLUTION_HZ,
        .gpio_num = config->tx_gpio,
        .mem_block_symbols = mem_block,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
    };
    ESP_GOTO_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &dev->tx_channel), err, TAG,
                      "Failed to create TX channel");

    const rmt_simple_encoder_config_t enc_cfg = {
        .callback = dali_tx_encode_cb,
        .arg = dev,
    };
    ESP_GOTO_ON_ERROR(rmt_new_simple_encoder(&enc_cfg, &dev->tx_encoder), err, TAG,
                      "Failed to create TX encoder");

    /* Both TX and RX channels are kept disabled at init and enabled on-demand
     * inside dali_master_do_raw_transaction(), saving power between transactions. */

    ESP_LOGI(TAG, "DALI driver initialized (RX GPIO %d%s, TX GPIO %d%s, mem_block=%" PRIu32 ")",
             config->rx_gpio, config->invert_rx ? " inv" : "", config->tx_gpio,
             config->invert_tx ? " inv" : "", mem_block);

    *handle = dev;
    return ESP_OK;

err:
    dali_del_master(dev);
    return ret;
}

esp_err_t dali_del_master(dali_master_handle_t handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }
    struct dali_master_t *dev = handle;

    if (dev->listen_task) {
        (void)dali_master_listen_stop(dev);
        dev->listen_quit = true;
        dali_listen_wake(dev);
        if (xSemaphoreTake(dev->listen_exited, pdMS_TO_TICKS(2 * DALI_LISTEN_MUX_WAIT_MS)) !=
            pdTRUE) {
            ESP_LOGE(TAG,
                     "listener task did not exit; leaking it rather than deleting it mid-RMT call");
            return ESP_ERR_TIMEOUT;
        }
        dev->listen_task = NULL;
    }

    if (dev->tx_channel) {
        rmt_del_channel(dev->tx_channel);
        dev->tx_channel = NULL;
    }
    if (dev->tx_encoder) {
        rmt_del_encoder(dev->tx_encoder);
        dev->tx_encoder = NULL;
    }
    if (dev->rx_channel) {
        rmt_del_channel(dev->rx_channel);
        dev->rx_channel = NULL;
    }
    if (dev->rx_queue) {
        vQueueDelete(dev->rx_queue);
        dev->rx_queue = NULL;
    }
    if (dev->listen_q) {
        vQueueDelete(dev->listen_q);
        dev->listen_q = NULL;
    }
    if (dev->listen_mux) {
        vSemaphoreDelete(dev->listen_mux);
        dev->listen_mux = NULL;
    }
    if (dev->listen_exited) {
        vSemaphoreDelete(dev->listen_exited);
        dev->listen_exited = NULL;
    }
    if (dev->listen_idle) {
        vSemaphoreDelete(dev->listen_idle);
        dev->listen_idle = NULL;
    }
    free(dev);
    ESP_LOGI(TAG, "DALI driver de-initialized");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Public API — transactions
 * ------------------------------------------------------------------------- */

esp_err_t dali_master_do_raw_transaction(dali_master_handle_t handle, const uint8_t *tx_buf,
                                         size_t tx_len, bool send_twice, int tx_timeout_ms,
                                         int *result)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle must not be NULL");
    ESP_RETURN_ON_FALSE(tx_buf != NULL && tx_len > 0, ESP_ERR_INVALID_ARG, TAG,
                        "tx buffer must not be empty");

    struct dali_master_t *dev = handle;
    const rmt_transmit_config_t tx_cfg = {.loop_count = 0};

    /* Take the RX channel away from the listener for the whole transaction. Costs two register
     * writes, so a scan is no slower with listening on than without. */
    ESP_RETURN_ON_ERROR(dali_listen_pause(dev, tx_timeout_ms), TAG,
                        "Failed to acquire the RX channel");

    xQueueReset(dev->rx_queue);

    /* Enable both channels at the start of each transaction; both are
     * disabled together at the end, keeping the enable/disable symmetrical. */
    esp_err_t ret = ESP_OK;
    bool tx_enabled = false;
    bool rx_enabled = false;
    ESP_GOTO_ON_ERROR(rmt_enable(dev->tx_channel), done, TAG, "Failed to enable TX channel");
    tx_enabled = true;
    ESP_GOTO_ON_ERROR(rmt_enable(dev->rx_channel), done, TAG, "Failed to enable RX channel");
    rx_enabled = true;

    /* --- TX ------------------------------------------------------------ */
    esp_err_t tx_err;
    tx_err = rmt_transmit(dev->tx_channel, dev->tx_encoder, tx_buf, tx_len, &tx_cfg);
    if (tx_err == ESP_OK) {
        tx_err = rmt_tx_wait_all_done(dev->tx_channel, tx_timeout_ms);
    }
    if (tx_err == ESP_OK && send_twice) {
        vTaskDelay(pdMS_TO_TICKS(DALI_SEND_TWICE_DELAY_MS));
        tx_err = rmt_transmit(dev->tx_channel, dev->tx_encoder, tx_buf, tx_len, &tx_cfg);
        if (tx_err == ESP_OK) {
            tx_err = rmt_tx_wait_all_done(dev->tx_channel, tx_timeout_ms);
        }
    }
    if (tx_err != ESP_OK) {
        ESP_LOGE(TAG, "TX failed: %s", esp_err_to_name(tx_err));
        ret = tx_err;
        goto done;
    }

    /* --- No backward frame expected: TX only --------------------------- */
    if (result == NULL) {
        goto done;
    }

    /* --- Backward frame expected: RX ----------------------------------- */
    /* The RX channel also sees the FF itself, but the decoder discards it
     * (a 17-symbol FF is not a valid 8-bit BF).  We keep reading events
     * until one decodes successfully or the deadline expires.              */
    esp_err_t rx_err = rmt_receive(dev->rx_channel, dev->rx_raw, sizeof(dev->rx_raw), &dev->rx_cfg);
    if (rx_err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive failed: %s", esp_err_to_name(rx_err));
        ret = rx_err;
        goto done;
    }

    rmt_rx_done_event_data_t rx_data;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DALI_BF_TIMEOUT_MS);
    bool bf_found = false;

    while (!bf_found) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            break;
        }
        TickType_t remaining = deadline - now;

        if (xQueueReceive(dev->rx_queue, &rx_data, remaining) != pdPASS) {
            break;
        }

        uint32_t frame = 0;
        uint8_t bit_count = 0;
        dali_decode_status_t dec =
            dali_frame_decode(rx_data.received_symbols, rx_data.num_symbols, &frame, &bit_count);
        /* Only an 8-bit frame is a reply; 16/24 bits is the echo of what we just sent. */
        if (dec == DALI_DECODE_OK && bit_count == 8) {
            *result = (int)frame;
            ESP_LOGD(TAG, "BF received: 0x%02X", (unsigned)frame);
            bf_found = true;
        } else {
            /* Could be the FF echo or a garbled frame — re-arm and keep waiting. */
            ESP_LOGD(TAG, "RX event discarded (status %d, bits %d, symbols %u) — waiting for BF",
                     (int)dec, bit_count, (unsigned)rx_data.num_symbols);
            esp_err_t rearm =
                rmt_receive(dev->rx_channel, dev->rx_raw, sizeof(dev->rx_raw), &dev->rx_cfg);
            if (rearm != ESP_OK) {
                ESP_LOGE(TAG, "rmt_receive re-arm failed: %s", esp_err_to_name(rearm));
                break;
            }
        }
    }

    if (!bf_found) {
        *result = DALI_RESULT_NO_REPLY;
        ESP_LOGD(TAG, "BF timeout — no reply");
    }

done:
    if (rx_enabled) {
        rmt_disable(dev->rx_channel);
    }
    if (tx_enabled) {
        rmt_disable(dev->tx_channel);
    }

    /* IEC 62386: next FF must be sent > 22 Te after this FF ends. */
    vTaskDelay(pdMS_TO_TICKS(DALI_IFG_MS));

    /* Resuming only after the gap means the listener is re-armed at a point where our own frame
     * is provably over, so an echo of it can never reach the user callback. */
    dali_listen_resume(dev);
    return ret;
}

esp_err_t dali_master_do_transaction(dali_master_handle_t handle,
                                     const dali_master_transaction_config_t *config, int *result)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config must not be NULL");

    dali_addr_type_t addr_type = config->addr_type;
    uint8_t addr = config->addr;
    bool is_cmd = config->is_cmd;
    uint8_t command = config->command;

    if (addr_type == DALI_ADDR_SHORT && addr > 63) {
        ESP_LOGE(TAG, "Short address out of range: %d", addr);
        return ESP_ERR_INVALID_ARG;
    }
    if (addr_type == DALI_ADDR_GROUP && addr > 15) {
        ESP_LOGE(TAG, "Group address out of range: %d", addr);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addr_byte;
    switch (addr_type) {
        case DALI_ADDR_SPECIAL:
            addr_byte = addr;
            break;
        case DALI_ADDR_BROADCAST:
            addr_byte = DALI_ADDR_MASK_BROADCAST | (is_cmd ? 0x01U : 0x00U);
            break;
        case DALI_ADDR_SHORT:
            addr_byte = DALI_ADDR_MASK_SHORT | (uint8_t)(addr << 1) | (is_cmd ? 0x01U : 0x00U);
            break;
        case DALI_ADDR_GROUP:
            addr_byte = DALI_ADDR_MASK_GROUP | (uint8_t)(addr << 1) | (is_cmd ? 0x01U : 0x00U);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    const uint8_t tx_buf[2] = {addr_byte, command};
    return dali_master_do_raw_transaction(handle, tx_buf, sizeof(tx_buf), config->send_twice,
                                          config->tx_timeout_ms, result);
}

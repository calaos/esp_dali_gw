/*
 * The single owner of the DALI bus (ADR 0001). One task drains the command queue and is the only
 * caller of the espressif/dali driver, which is blocking and not thread-safe.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "dali_bus.h"
#include "dali_bus_priv.h"
#include "gw_events.h"

static const char *TAG = "bus";

#define BUS_TASK_PRIO 10 /**< above the Wi-Fi/lwIP app tasks so the reply window is served */
#define BUS_TASK_STACK 6144

/** Refcounted rendezvous for dali_bus_submit_sync(): either side may finish first. */
typedef struct {
    SemaphoreHandle_t done;
    gw_result_t res;
    atomic_int refs;
} bus_reply_t;

typedef struct {
    gw_cmd_t cmd;
    bus_reply_t *reply; /**< NULL for fire-and-forget submissions */
} bus_msg_t;

/** Next automatic poll, in esp_timer units; 0 while polling is disabled. */
static int64_t s_next_poll_us;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_registry_lock;
static dali_bus_config_t s_cfg;
static bus_ctx_t s_ctx;
static atomic_bool s_cancel_requested;
static atomic_bool s_listen_wanted;

/* --- reply plumbing -------------------------------------------------------------------------- */

static bus_reply_t *reply_create(void)
{
    bus_reply_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->done = xSemaphoreCreateBinary();
    if (r->done == NULL) {
        free(r);
        return NULL;
    }
    atomic_init(&r->refs, 2);
    return r;
}

static void reply_release(bus_reply_t *r)
{
    if (atomic_fetch_sub(&r->refs, 1) == 1) {
        gw_api_result_free(&r->res);
        vSemaphoreDelete(r->done);
        free(r);
    }
}

/* --- registry -------------------------------------------------------------------------------- */

static void registry_lock(void)
{
    xSemaphoreTake(s_registry_lock, portMAX_DELAY);
}

static void registry_unlock(void)
{
    xSemaphoreGive(s_registry_lock);
}

void bus_notify_gear(uint8_t addr)
{
    const gw_event_gear_t ev = {.addr = addr};
    gw_event_post(GW_EVENT_GEAR_CHANGED, &ev, sizeof(ev));
}

void bus_notify_state(const bus_ctx_t *ctx)
{
    const gw_event_bus_t ev = {
        .powered = ctx->powered,
        .busy = ctx->op.kind != GW_CMD_NONE,
    };
    gw_event_post(GW_EVENT_BUS_STATE, &ev, sizeof(ev));
}

/* --- driver access --------------------------------------------------------------------------- */

static dali_addr_type_t map_addr_type(gw_target_type_t type)
{
    switch (type) {
        case GW_TARGET_GROUP:
            return DALI_ADDR_GROUP;
        case GW_TARGET_BROADCAST:
            return DALI_ADDR_BROADCAST;
        case GW_TARGET_SHORT:
        default:
            return DALI_ADDR_SHORT;
    }
}

/** Longest we defer to a foreign master before transmitting anyway. */
#define IDLE_WAIT_MS 20
#define IDLE_SLICE_MS 2

/*
 * pdMS_TO_TICKS() truncates, and the default tick is 100 Hz: every delay under 10 ms rounds to
 * zero, which turns vTaskDelay into a bare yield and any "wait a little" loop into a busy spin at
 * priority 10. Always round a non-zero millisecond count up to at least one tick.
 */
#define TICKS_AT_LEAST_ONE(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : (TickType_t)1)

/*
 * DALI has no collision detection, so the only courtesy available is to wait for quiet. The wait is
 * bounded and then the frame goes out regardless: refusing would turn a miscalibrated idle
 * threshold into a device that cannot drive its own bus, and DALI_LISTEN_IDLE_US has never been
 * measured against a real receiver. A check that can only improve things, never brick them.
 */
static void wait_for_idle(bus_ctx_t *ctx)
{
    /*
     * Only while listening. Without it the driver sees nothing but its own transmissions, so the
     * check adds no information -- and if the idle threshold turns out to be miscalibrated on real
     * hardware, gating it here keeps the cost inside the diagnostic mode instead of adding up to
     * IDLE_WAIT_MS to every frame of an ordinary scan.
     */
    if (!atomic_load(&s_listen_wanted)) {
        return;
    }

    for (int waited = 0; waited < IDLE_WAIT_MS; waited += IDLE_SLICE_MS) {
        if (dali_master_bus_idle(ctx->master)) {
            return;
        }
        vTaskDelay(TICKS_AT_LEAST_ONE(IDLE_SLICE_MS));
    }
    ESP_LOGD(TAG, "bus still busy after %d ms, transmitting anyway", IDLE_WAIT_MS);
}

gw_err_t bus_transact(bus_ctx_t *ctx, gw_target_t target, bool is_cmd, uint8_t opcode,
                      bool send_twice, int *reply)
{
    if (ctx->master == NULL) {
        return GW_ERR_INTERNAL;
    }
    wait_for_idle(ctx);

    const dali_master_transaction_config_t cfg = {
        .addr_type = map_addr_type(target.type),
        .addr = target.addr,
        .is_cmd = is_cmd,
        .command = opcode,
        .send_twice = send_twice,
        .tx_timeout_ms = BUS_TX_TIMEOUT_MS,
    };

    /*
     * Passing NULL through tells the driver to skip the backward-frame window entirely (it returns
     * right after TX). Handing it a scratch pointer instead would make every command frame -- DAPC,
     * STORE, TERMINATE -- wait 25 ms for a reply that cannot come.
     */
    int got = DALI_RESULT_NO_REPLY;
    esp_err_t err = dali_master_do_transaction(ctx->master, &cfg, reply != NULL ? &got : NULL);
    gw_event_post(GW_EVENT_BUS_ACTIVITY, NULL, 0);

    if (err != ESP_OK) {
        return gw_api_err_from_esp(err);
    }
    if (reply != NULL) {
        *reply = got;
    }
    return GW_OK;
}

gw_err_t bus_special(bus_ctx_t *ctx, uint8_t special, uint8_t data, bool send_twice, int *reply)
{
    if (ctx->master == NULL) {
        return GW_ERR_INTERNAL;
    }
    wait_for_idle(ctx);

    const dali_master_transaction_config_t cfg = {
        .addr_type = DALI_ADDR_SPECIAL,
        .addr = special,
        .is_cmd = true,
        .command = data,
        .send_twice = send_twice,
        .tx_timeout_ms = BUS_TX_TIMEOUT_MS,
    };
    int got = DALI_RESULT_NO_REPLY;
    esp_err_t err = dali_master_do_transaction(ctx->master, &cfg, reply != NULL ? &got : NULL);
    gw_event_post(GW_EVENT_BUS_ACTIVITY, NULL, 0);
    if (err != ESP_OK) {
        return gw_api_err_from_esp(err);
    }
    if (reply != NULL) {
        *reply = got;
    }
    return GW_OK;
}

/* --- result delivery ------------------------------------------------------------------------- */

static void publish_result(const gw_result_t *res)
{
    gw_event_result_t ev = {
        .id = res->id,
        .ok = res->ok,
        .error = res->error,
        .target = res->target,
        .has_target = res->has_target,
        .duration_ms = res->duration_ms,
    };
    strlcpy(ev.action, res->action, sizeof(ev.action));
    gw_event_post(GW_EVENT_RESULT, &ev, sizeof(ev));
}

/** Hands the result to a waiting caller, or publishes and discards it. Takes ownership of res. */
static void deliver(bus_msg_t *msg, gw_result_t *res)
{
    publish_result(res);
    if (msg->reply != NULL) {
        msg->reply->res = *res;
        memset(res, 0, sizeof(*res)); /* data ownership moved to the waiter */
        xSemaphoreGive(msg->reply->done);
        reply_release(msg->reply);
    } else {
        gw_api_result_free(res);
    }
}

static void reject(bus_msg_t *msg, gw_err_t err, const char *message)
{
    gw_result_t res = {.id = msg->cmd.id, .ok = false, .error = err};
    strlcpy(res.message, message, sizeof(res.message));
    res.has_target = true;
    res.target = msg->cmd.target;
    deliver(msg, &res);
}

/* --- task ------------------------------------------------------------------------------------ */

static bool is_long_operation(gw_cmd_kind_t kind)
{
    return kind == GW_CMD_SCAN || kind == GW_CMD_COMMISSION || kind == GW_CMD_POLL_ALL ||
           kind == GW_CMD_CONFIGURE || kind == GW_CMD_IDENTIFY;
}

/**
 * configure runs as a state machine because a full write is over a hundred frames, but the client
 * expects the real outcome, not an acknowledgement (SPEC 9 lists it among the synchronous routes).
 * Its caller is therefore parked until the operation ends instead of being answered up front.
 */
static bool holds_caller_until_done(gw_cmd_kind_t kind)
{
    return kind == GW_CMD_CONFIGURE;
}

static void run_command(bus_msg_t *msg)
{
    gw_result_t res;
    memset(&res, 0, sizeof(res));

    registry_lock();
    bus_exec_short(&s_ctx, &msg->cmd, &res);
    registry_unlock();

    deliver(msg, &res);
}

/**
 * The started acknowledgement goes to whoever asked, but it must not be published as a result: it
 * carries the same id and action as the real one, so a client watching the stream would see an
 * indistinguishable "scan ok" at t=0 and again when the scan actually finishes.
 */
static void start_long(bus_msg_t *msg)
{
    gw_result_t res;
    memset(&res, 0, sizeof(res));

    registry_lock();
    bus_long_begin(&s_ctx, &msg->cmd, &res);
    s_ctx.op.reply_handle = NULL;
    if (res.ok && holds_caller_until_done(msg->cmd.kind)) {
        s_ctx.op.reply_handle = msg->reply;
        msg->reply = NULL; /* ownership moves to the operation */
    }
    registry_unlock();

    if (msg->reply != NULL) {
        msg->reply->res = res;
        memset(&res, 0, sizeof(res));
        xSemaphoreGive(msg->reply->done);
        reply_release(msg->reply);
    } else {
        gw_api_result_free(&res);
    }
    bus_notify_state(&s_ctx);
}

/** Hand a finished long operation's result to the caller parked on it, if any. */
static void finish_long(gw_result_t *res)
{
    publish_result(res);

    bus_reply_t *waiter = s_ctx.op.reply_handle;
    s_ctx.op.reply_handle = NULL;
    if (waiter != NULL) {
        waiter->res = *res;
        memset(res, 0, sizeof(*res));
        xSemaphoreGive(waiter->done);
        reply_release(waiter);
    } else {
        gw_api_result_free(res);
    }
}

/**
 * Between two steps of a long operation the queue is drained so the UI stays responsive: cancel and
 * the urgent level commands are served immediately, and exactly one ordinary command per step so
 * that a 4 s scan cannot starve a gear query without stalling the scan either (SPEC 7.2).
 */
static void serve_between_steps(void)
{
    bus_msg_t msg;
    bool served_ordinary = false;

    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
        if (msg.cmd.kind == GW_CMD_CANCEL) {
            s_ctx.op.cancel = true;
            gw_result_t res = {.id = msg.cmd.id, .ok = true};
            strlcpy(res.action, "cancel", sizeof(res.action));
            deliver(&msg, &res);
            continue;
        }
        if (is_long_operation(msg.cmd.kind)) {
            char message[96];
            snprintf(message, sizeof(message), "%s is running",
                     s_ctx.op.kind == GW_CMD_SCAN ? "scan" : "another operation");
            reject(&msg, GW_ERR_BUS_BUSY, message);
            continue;
        }
        if (s_ctx.op.kind == GW_CMD_COMMISSION) {
            /* Gears are in the initialise state and ignore ordinary commands, so serving one would
             * report a success that never reached the bus. */
            reject(&msg, GW_ERR_BUS_BUSY, "commissioning is running");
            continue;
        }
        if (msg.cmd.urgent) {
            run_command(&msg);
            continue;
        }
        if (!served_ordinary) {
            run_command(&msg);
            served_ordinary = true;
            continue;
        }
        /* Put it back and stop draining: it keeps its place and the step goes ahead. */
        if (xQueueSendToFront(s_queue, &msg, 0) != pdTRUE) {
            reject(&msg, GW_ERR_BUS_BUSY, "queue full");
        }
        break;
    }
}

static void bus_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (s_ctx.op.kind != GW_CMD_NONE) {
            /* A full queue means dali_bus_cancel() could not enqueue; the flag is the fallback. */
            if (atomic_load(&s_cancel_requested)) {
                s_ctx.op.cancel = true;
            }
            serve_between_steps();

            bool done = false;
            gw_result_t res;
            memset(&res, 0, sizeof(res));

            registry_lock();
            bus_long_step(&s_ctx, &done, &res);
            registry_unlock();

            if (done) {
                bool rescan = s_ctx.op.kind == GW_CMD_COMMISSION && !s_ctx.op.cancel;
                finish_long(&res);
                s_ctx.op.kind = GW_CMD_NONE;
                if (rescan) {
                    /* SPEC 7.4: commissioning is followed by a scan. Without it the registry
                     * still describes the addresses the gears had before. */
                    gw_cmd_t scan = {.kind = GW_CMD_SCAN, .origin = GW_ORIGIN_INTERNAL};
                    dali_bus_submit(&scan);
                }
                atomic_store(&s_cancel_requested, false);
                bus_notify_state(&s_ctx);
            }
            continue;
        }

        /*
         * Wait until the next poll is due rather than ticking every second: with polling disabled
         * the task sleeps until something is actually submitted.
         */
        TickType_t wait = portMAX_DELAY;
        if (s_next_poll_us != 0) {
            int64_t remaining_ms = (s_next_poll_us - esp_timer_get_time()) / 1000;
            wait = remaining_ms > 0 ? TICKS_AT_LEAST_ONE(remaining_ms) : 0;
        }

        bus_msg_t msg;
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            if (msg.cmd.kind == GW_CMD_CANCEL) {
                gw_result_t res = {.id = msg.cmd.id, .ok = true};
                strlcpy(res.action, "cancel", sizeof(res.action));
                deliver(&msg, &res);
            } else if (is_long_operation(msg.cmd.kind)) {
                start_long(&msg);
            } else {
                run_command(&msg);
            }
            continue;
        }

        /* Nothing queued and the poll fell due. A poll is skipped silently when the bus is
         * unpowered: 64 timeouts every interval would fill the log and buy nothing. */
        if (s_next_poll_us != 0 && esp_timer_get_time() >= s_next_poll_us) {
            s_next_poll_us = esp_timer_get_time() + (int64_t)s_cfg.poll_interval_s * 1000000;

            if (s_ctx.powered) {
                bus_msg_t poll = {.cmd = {.kind = GW_CMD_POLL_ALL, .origin = GW_ORIGIN_INTERNAL},
                                  .reply = NULL};
                start_long(&poll);
            } else {
                /* Nothing else re-probes: a bus whose PSU is switched on after boot would stay
                 * "unpowered" for ever, and polling with it. One cheap frame per interval. */
                bus_msg_t check = {.cmd = {.kind = GW_CMD_BUS_CHECK, .origin = GW_ORIGIN_INTERNAL},
                                   .reply = NULL};
                run_command(&check);
            }
        }
    }
}

/* --- listen mode ------------------------------------------------------------------------------ */

/**
 * Called from the driver's listener task, not an ISR, but still on the driver's stack: it must do
 * nothing but hand the frame on. A busy event loop drops it, which is the right trade -- a monitor
 * that stalls the receiver is worse than a monitor that misses a frame under load.
 */
static void on_rx_frame(const dali_rx_frame_t *frame, void *user_data)
{
    (void)user_data;
    const gw_event_rx_t ev = {
        .frame = frame->frame,
        .bits = frame->bits,
        .timestamp_us = frame->timestamp_us,
    };
    gw_event_post(GW_EVENT_RX, &ev, sizeof(ev));
}

esp_err_t dali_bus_listen_set(bool enable)
{
    if (s_ctx.master == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == atomic_load(&s_listen_wanted)) {
        return ESP_OK;
    }

    esp_err_t err = enable ? dali_master_listen_start(s_ctx.master, on_rx_frame, NULL)
                           : dali_master_listen_stop(s_ctx.master);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "listen %s failed: %s", enable ? "start" : "stop", esp_err_to_name(err));
        return err;
    }
    atomic_store(&s_listen_wanted, enable);
    ESP_LOGW(TAG, "passive listening %s", enable ? "on" : "off");
    return ESP_OK;
}

bool dali_bus_listen_active(void)
{
    return atomic_load(&s_listen_wanted);
}

/* --- public API ------------------------------------------------------------------------------ */

esp_err_t dali_bus_init(const dali_bus_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "no config");
    s_cfg = *cfg;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.identify_blink_ms = s_cfg.identify_blink_ms;
    for (uint8_t i = 0; i < GW_MAX_GEARS; i++) {
        s_ctx.gears[i].addr = i;
    }

    s_queue = xQueueCreate(DALI_BUS_QUEUE_DEPTH, sizeof(bus_msg_t));
    s_registry_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_queue != NULL && s_registry_lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "queue alloc");

    const dali_master_config_t master_cfg = {
        .rx_gpio = (gpio_num_t)s_cfg.rx_gpio,
        .tx_gpio = (gpio_num_t)s_cfg.tx_gpio,
        .invert_tx = s_cfg.invert_tx,
        .invert_rx = s_cfg.invert_rx,
    };
    /* 0 = auto: the C6's RMT channels hold 48 symbols, not the 64 the driver would assume. */
    const dali_master_rmt_config_t rmt_cfg = {.mem_block_symbols = 0};

    esp_err_t err = dali_new_master_rmt(&master_cfg, &rmt_cfg, &s_ctx.master);
    if (err != ESP_OK) {
        /* A bad GPIO must not stop the device from serving its UI so the user can fix it. */
        ESP_LOGE(TAG, "DALI master on tx=%d rx=%d failed: %s; bus disabled", s_cfg.tx_gpio,
                 s_cfg.rx_gpio, esp_err_to_name(err));
        s_ctx.master = NULL;
    }

    ESP_RETURN_ON_FALSE(
        xTaskCreate(bus_task, "dali_bus", BUS_TASK_STACK, NULL, BUS_TASK_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "task create");

    if (s_cfg.poll_interval_s > 0) {
        s_next_poll_us = esp_timer_get_time() + (int64_t)s_cfg.poll_interval_s * 1000000;
    }

    if (s_cfg.scan_on_boot) {
        gw_cmd_t scan = {.kind = GW_CMD_SCAN, .origin = GW_ORIGIN_INTERNAL};
        dali_bus_submit(&scan);
    }

    ESP_LOGI(TAG, "bus task up (tx=%d rx=%d, poll %us)", s_cfg.tx_gpio, s_cfg.rx_gpio,
             s_cfg.poll_interval_s);
    return ESP_OK;
}

esp_err_t dali_bus_submit(const gw_cmd_t *cmd)
{
    ESP_RETURN_ON_FALSE(cmd != NULL && s_queue != NULL, ESP_ERR_INVALID_ARG, TAG, "bad submit");
    bus_msg_t msg = {.cmd = *cmd, .reply = NULL};
    /* Never block an adapter task on the bus: a full queue is bus_busy, reported by the caller. */
    return xQueueSend(s_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t dali_bus_submit_sync(const gw_cmd_t *cmd, gw_result_t *res, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(cmd != NULL && res != NULL && s_queue != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "bad submit");

    bus_reply_t *reply = reply_create();
    ESP_RETURN_ON_FALSE(reply != NULL, ESP_ERR_NO_MEM, TAG, "reply alloc");

    bus_msg_t msg = {.cmd = *cmd, .reply = reply};
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        reply_release(reply); /* drop our reference and the bus task's: it never saw the message */
        reply_release(reply);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(reply->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        reply_release(reply); /* the bus task still holds a reference and will free it */
        return ESP_ERR_TIMEOUT;
    }

    *res = reply->res;
    memset(&reply->res, 0, sizeof(reply->res)); /* data ownership moves to the caller */
    reply_release(reply);
    return ESP_OK;
}

esp_err_t dali_bus_cancel(void)
{
    atomic_store(&s_cancel_requested, true);
    gw_cmd_t cmd = {.kind = GW_CMD_CANCEL, .urgent = true};
    return dali_bus_submit(&cmd);
}

void dali_bus_get_status(gw_bus_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    registry_lock();
    out->powered = s_ctx.powered;
    out->busy = s_ctx.op.kind != GW_CMD_NONE;
    out->last_scan = s_ctx.last_scan;
    if (out->busy) {
        out->has_progress = true;
        out->done = s_ctx.op.cursor;
        out->total = s_ctx.op.total;
        out->found = s_ctx.op.found;
        strlcpy(out->operation,
                s_ctx.op.kind == GW_CMD_SCAN         ? "scan"
                : s_ctx.op.kind == GW_CMD_COMMISSION ? "commission"
                                                     : "poll_all",
                sizeof(out->operation));
    }
    for (uint8_t i = 0; i < GW_MAX_GEARS; i++) {
        if (s_ctx.gears[i].present) {
            out->gear_count++;
        }
    }
    registry_unlock();
}

esp_err_t dali_bus_get_gear(uint8_t addr, gw_gear_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL && addr < GW_MAX_GEARS, ESP_ERR_INVALID_ARG, TAG, "bad addr");
    registry_lock();
    *out = s_ctx.gears[addr];
    registry_unlock();
    return ESP_OK;
}

esp_err_t dali_bus_get_gears(gw_gear_t *out, size_t max, size_t *count)
{
    ESP_RETURN_ON_FALSE(out != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    size_t n = 0;
    registry_lock();
    for (uint8_t i = 0; i < GW_MAX_GEARS && n < max; i++) {
        if (s_ctx.gears[i].present) {
            out[n++] = s_ctx.gears[i];
        }
    }
    registry_unlock();
    *count = n;
    return ESP_OK;
}

esp_err_t dali_bus_set_gear_name(uint8_t addr, const char *name)
{
    ESP_RETURN_ON_FALSE(addr < GW_MAX_GEARS, ESP_ERR_INVALID_ARG, TAG, "bad addr");
    registry_lock();
    strlcpy(s_ctx.gears[addr].name, name != NULL ? name : "", sizeof(s_ctx.gears[addr].name));
    registry_unlock();
    bus_notify_gear(addr);
    return ESP_OK;
}

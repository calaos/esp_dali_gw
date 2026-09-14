/*
 * Part 102 commissioning as a resumable state machine (ADR 0004).
 *
 * The driver's dali_commission() runs the whole binary search in one blocking call, which for a
 * full bus is minutes with no progress and no cancel. This does the same sequence one transaction
 * per step so the bus task can serve the queue -- and a cancel -- between them.
 *
 * Sequence per IEC 62386-102: TERMINATE, INITIALISE, RANDOMISE, then repeatedly bisect the 24-bit
 * random address space with SEARCHADDR + COMPARE until one device is isolated, PROGRAM and VERIFY
 * its short address, WITHDRAW it from the search, and repeat until COMPARE finds nothing.
 */
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dali_bus_priv.h"
#include "gw_events.h"

static const char *TAG = "bus";

/** IEC 62386-102: RANDOMISE needs at least 100 ms before the first COMPARE is meaningful. */
#define RANDOMISE_SETTLE_MS 120

/** Time for input devices to act on the Part 103 TERMINATE before the 102 search. */
#define QUIESCENT_SETTLE_MS 50

/** Rounds without an assignment before we conclude a device will never withdraw. */
#define COMM_MAX_STUCK_ROUNDS 3

/** Bounded so a cancel is still served promptly while the settle time runs out. */
#define SETTLE_SLICE_MS 10

/** pdMS_TO_TICKS truncates at a 100 Hz tick; a sub-tick delay is a spin, not a wait. */
#define TICKS_AT_LEAST_ONE(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : (TickType_t)1)

/** Part 102 INITIALISE data byte. The driver's enum names the modes; these are the wire values. */
#define INIT_ALL 0x00
#define INIT_UNADDRESSED 0xFF

#define SEARCH_MAX 0xFFFFFFu

static void set_action(gw_result_t *res, const char *action)
{
    strlcpy(res->action, action, sizeof(res->action));
}

void bus_commission_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    res->id = cmd->id;
    set_action(res, "commission");

    if (cmd->args.commission.mode == GW_COMMISSION_ALL && !cmd->args.commission.confirm) {
        ctx->op.kind = GW_CMD_NONE;
        res->ok = false;
        res->error = GW_ERR_INVALID_ARG;
        strlcpy(res->message, "re-addressing the whole bus needs confirm", sizeof(res->message));
        return;
    }

    comm_ctx_t *c = &ctx->op.comm;
    memset(c, 0, sizeof(*c));
    c->state = COMM_QUIESCENT;
    c->mode = cmd->args.commission.mode;
    c->next_addr = cmd->args.commission.start_addr;
    c->max_devices = (uint8_t)(GW_MAX_GEARS - cmd->args.commission.start_addr);

    if (cmd->args.commission.mode == GW_COMMISSION_ALL) {
        /* Every short address is about to be reassigned, so the registry describes a bus that no
         * longer exists. Names are configuration and survive; cached bus state does not. */
        for (uint8_t a = 0; a < GW_MAX_GEARS; a++) {
            char kept[GW_NAME_LEN];
            strlcpy(kept, ctx->gears[a].name, sizeof(kept));
            memset(&ctx->gears[a], 0, sizeof(gw_gear_t));
            ctx->gears[a].addr = a;
            strlcpy(ctx->gears[a].name, kept, sizeof(ctx->gears[a].name));
        }
    }

    ctx->op.total = c->max_devices;

    ESP_LOGW(TAG, "commissioning started (%s, from short address %u)",
             cmd->args.commission.mode == GW_COMMISSION_ALL ? "all" : "unaddressed", c->next_addr);

    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data != NULL) {
        cJSON_AddBoolToObject(res->data, "started", true);
    }
}

static void finish(bus_ctx_t *ctx, bool *done, gw_result_t *res, gw_err_t err, const char *message)
{
    *done = true;
    res->id = ctx->op.id;
    set_action(res, "commission");
    res->duration_ms = (uint32_t)((esp_timer_get_time() - ctx->op.started_us) / 1000);

    if (err != GW_OK) {
        res->ok = false;
        res->error = err;
        strlcpy(res->message, message, sizeof(res->message));
        return;
    }
    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data != NULL) {
        cJSON_AddNumberToObject(res->data, "assigned", ctx->op.found);
        cJSON *list = cJSON_AddArrayToObject(res->data, "addresses");
        if (list != NULL) {
            for (uint8_t a = 0; a < GW_MAX_GEARS; a++) {
                if (ctx->gears[a].present) {
                    cJSON_AddItemToArray(list, cJSON_CreateNumber(a));
                }
            }
        }
    }
}

/** Write only the SEARCHADDR bytes that actually differ; the high byte rarely moves. */
static bool write_next_search_byte(bus_ctx_t *ctx)
{
    comm_ctx_t *c = &ctx->op.comm;

    while (c->byte_index < 3) {
        uint8_t shift = (uint8_t)(16 - 8 * c->byte_index);
        uint8_t want = (uint8_t)(c->search >> shift);
        uint8_t have = (uint8_t)(c->written >> shift);
        uint8_t special = c->byte_index == 0   ? DALI_SPECIAL_SEARCH_ADDR_H
                          : c->byte_index == 1 ? DALI_SPECIAL_SEARCH_ADDR_M
                                               : DALI_SPECIAL_SEARCH_ADDR_L;
        c->byte_index++;

        if (c->written_valid && want == have) {
            continue;
        }
        bus_special(ctx, special, want, false, NULL);
        return true; /* one transaction spent */
    }

    c->written = c->search;
    c->written_valid = true;
    return false; /* nothing left to write */
}

/**
 * A cancel must not just stop the loop: leaving the bus in the initialised state makes every gear
 * ignore ordinary commands for 15 minutes. TERMINATE is sent before giving up.
 */
/**
 * Input devices were silenced before the search; leaving them that way outlasts us. The standard's
 * own timeout is 15 minutes, so a gateway that forgets this leaves every wall switch in the
 * installation dead for a quarter of an hour after each commissioning run.
 */
static void leave_quiescent(bus_ctx_t *ctx)
{
    if (ctx->master != NULL) {
        dali_103_do_device_command(ctx->master, DALI_ADDR_BROADCAST, 0,
                                   DALI_103_STOP_QUIESCENT_MODE, true, BUS_TX_TIMEOUT_MS, NULL);
        gw_event_post(GW_EVENT_BUS_ACTIVITY, NULL, 0);
    }
}

static void cancel_now(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    bus_special(ctx, DALI_SPECIAL_TERMINATE, 0, false, NULL);
    leave_quiescent(ctx);
    ESP_LOGW(TAG, "commissioning cancelled after %u assignment(s)", ctx->op.found);
    finish(ctx, done, res, GW_ERR_CANCELLED, "cancelled");
}

void bus_commission_step(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    comm_ctx_t *c = &ctx->op.comm;
    int reply = DALI_RESULT_NO_REPLY;

    if (ctx->op.cancel && c->state != COMM_TERMINATE_END) {
        cancel_now(ctx, done, res);
        return;
    }

    switch (c->state) {
        case COMM_QUIESCENT:
            /* Input devices must stop emitting, or their frames land in the COMPARE reply windows
             * and the search sees phantom devices (ADR 0004). */
            if (ctx->master != NULL) {
                dali_103_do_device_command(ctx->master, DALI_ADDR_BROADCAST, 0,
                                           DALI_103_START_QUIESCENT_MODE, true, BUS_TX_TIMEOUT_MS,
                                           NULL);
                gw_event_post(GW_EVENT_BUS_ACTIVITY, NULL, 0);
            }
            c->settle_until_us = esp_timer_get_time() + QUIESCENT_SETTLE_MS * 1000;
            c->state = COMM_QUIESCENT_SETTLE;
            break;

        case COMM_QUIESCENT_SETTLE:
            if (esp_timer_get_time() < c->settle_until_us) {
                vTaskDelay(TICKS_AT_LEAST_ONE(SETTLE_SLICE_MS));
                break;
            }
            c->state = COMM_103_TERMINATE;
            break;

        case COMM_103_TERMINATE:
            /* Quiescent mode silences event frames, but an input device already in Part 103
             * commissioning still answers COMPARE and appears as a phantom gear. */
            if (ctx->master != NULL) {
                dali_103_send_special(ctx->master, DALI_103_SPECIAL_TERMINATE, 0x00U, false,
                                      BUS_TX_TIMEOUT_MS, NULL);
                gw_event_post(GW_EVENT_BUS_ACTIVITY, NULL, 0);
            }
            c->settle_until_us = esp_timer_get_time() + QUIESCENT_SETTLE_MS * 1000;
            c->state = COMM_103_SETTLE;
            break;

        case COMM_103_SETTLE:
            if (esp_timer_get_time() < c->settle_until_us) {
                vTaskDelay(TICKS_AT_LEAST_ONE(SETTLE_SLICE_MS));
                break;
            }
            c->state = COMM_TERMINATE_START;
            break;

        case COMM_TERMINATE_START:
            bus_special(ctx, DALI_SPECIAL_TERMINATE, 0, false, NULL);
            c->state = COMM_INITIALISE;
            break;

        case COMM_INITIALISE:
            bus_special(ctx, DALI_SPECIAL_INITIALIZE,
                        c->mode == GW_COMMISSION_ALL ? INIT_ALL : INIT_UNADDRESSED, true, NULL);
            c->state = COMM_RANDOMISE;
            break;

        case COMM_RANDOMISE:
            bus_special(ctx, DALI_SPECIAL_RANDOMIZE, 0, true, NULL);
            c->settle_until_us = esp_timer_get_time() + RANDOMISE_SETTLE_MS * 1000;
            c->state = COMM_SETTLE;
            break;

        case COMM_SETTLE:
            /* Sliced rather than one long delay so the queue is checked while we wait. */
            if (esp_timer_get_time() < c->settle_until_us) {
                vTaskDelay(TICKS_AT_LEAST_ONE(SETTLE_SLICE_MS));
                break;
            }
            c->state = COMM_ROUND_START;
            break;

        case COMM_ROUND_START:
            /*
             * Every round must either assign an address or remove a device from the pool. A device
             * that answers COMPARE and then ignores WITHDRAW -- a Part 103 input device caught in
             * the Part 102 search is the documented case -- would otherwise be isolated for ever,
             * spinning the bus task at full speed with no way out but a user cancel.
             */
            if (c->rounds > 0 && ctx->op.found == c->last_found) {
                c->stuck++;
                if (c->stuck >= COMM_MAX_STUCK_ROUNDS) {
                    bus_special(ctx, DALI_SPECIAL_TERMINATE, 0, false, NULL);
                    leave_quiescent(ctx);
                    finish(ctx, done, res, GW_ERR_INTERNAL,
                           "a device answers the search but will not take an address");
                    return;
                }
            } else {
                c->stuck = 0;
            }
            c->rounds++;
            c->last_found = ctx->op.found;

            c->low = 0;
            c->high = SEARCH_MAX;
            c->search = SEARCH_MAX;
            c->byte_index = 0;
            c->round_open = false;
            c->state = COMM_WRITE_SEARCH;
            break;

        case COMM_WRITE_SEARCH:
            if (!write_next_search_byte(ctx)) {
                c->state = COMM_COMPARE;
            }
            break;

        case COMM_COMPARE: {
            bus_special(ctx, DALI_SPECIAL_COMPARE, 0, false, &reply);
            /* A collision is a yes: several devices answered at once. */
            bool any = DALI_RESULT_IS_ACTIVITY(reply);

            if (!c->round_open) {
                /* First probe of a round, at the top of the range: nothing answering means the
                 * bus is fully commissioned. */
                if (!any) {
                    c->state = COMM_TERMINATE_END;
                    break;
                }
                c->round_open = true;
            }

            if (c->low < c->high) {
                if (any) {
                    c->high = c->search;
                } else {
                    c->low = c->search + 1;
                }
                c->search = c->low + (c->high - c->low) / 2;
                c->byte_index = 0;
                c->state = COMM_WRITE_SEARCH;
                break;
            }

            /* Converged: c->low is the lowest remaining random address. */
            c->search = c->low;
            c->byte_index = 0;
            c->state = COMM_PROGRAM;
            break;
        }

        case COMM_PROGRAM:
            /*
             * Skip addresses that are already taken. "unaddressed" on an existing installation is
             * the normal way to add one lamp, and start_addr defaults to 0 -- without this the new
             * gear lands on top of the gear already at 0, which op_set_short_address refuses to
             * create and which cannot be untangled from the UI.
             */
            while (c->next_addr < GW_MAX_GEARS && ctx->gears[c->next_addr].present) {
                c->next_addr++;
            }
            if (c->next_addr >= GW_MAX_GEARS) {
                bus_special(ctx, DALI_SPECIAL_TERMINATE, 0, false, NULL);
                leave_quiescent(ctx);
                finish(ctx, done, res, GW_ERR_ADDRESS_IN_USE, "ran out of short addresses");
                return;
            }
            /* The short address travels shifted left with bit 0 set (IEC 62386-102). */
            bus_special(ctx, DALI_SPECIAL_PROGRAM_SHORT_ADDR, (uint8_t)((c->next_addr << 1) | 1),
                        false, NULL);
            c->state = COMM_VERIFY;
            break;

        case COMM_VERIFY:
            bus_special(ctx, DALI_SPECIAL_VERIFY_SHORT_ADDR, (uint8_t)((c->next_addr << 1) | 1),
                        false, &reply);
            if (DALI_RESULT_IS_VALID(reply) && (uint8_t)reply == 0xFF) {
                ctx->gears[c->next_addr].present = true;
                ctx->gears[c->next_addr].last_seen = time(NULL);
                bus_notify_gear(c->next_addr);
                ctx->op.found++;
                ctx->op.cursor = ctx->op.found;
                c->next_addr++;
            } else {
                /* The device selected itself for COMPARE but did not take the address. Withdraw it
                 * anyway, or the next round isolates the same one forever. */
                ESP_LOGW(TAG, "gear did not verify short address %u", c->next_addr);
            }
            c->state = COMM_WITHDRAW;
            break;

        case COMM_WITHDRAW:
            bus_special(ctx, DALI_SPECIAL_WITHDRAW, 0, false, NULL);
            /* WITHDRAW only removes devices whose random address equals SEARCHADDR, so the search
             * registers must still hold the isolated value here -- they do. */
            c->state = COMM_ROUND_START;
            break;

        case COMM_TERMINATE_END:
        default:
            bus_special(ctx, DALI_SPECIAL_TERMINATE, 0, false, NULL);
            leave_quiescent(ctx);
            ESP_LOGW(TAG, "commissioning done, %u gear(s) addressed", ctx->op.found);
            finish(ctx, done, res, GW_OK, NULL);
            return;
    }

    /* Progress is per assigned device: the bit-level search is far too fast to be worth showing. */
    int64_t now = esp_timer_get_time();
    if (now - ctx->op.last_progress_us >= BUS_PROGRESS_INTERVAL_MS * 1000) {
        ctx->op.last_progress_us = now;
        gw_event_progress_t ev = {
            .done = ctx->op.found,
            .total = ctx->op.total,
            .found = ctx->op.found,
        };
        strlcpy(ev.operation, "commission", sizeof(ev.operation));
        gw_event_post(GW_EVENT_PROGRESS, &ev, sizeof(ev));
    }
}

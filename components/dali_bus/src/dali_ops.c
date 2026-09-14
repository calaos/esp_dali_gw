/*
 * Command execution. Short commands cost a bounded number of transactions and run inline; long
 * operations advance one transaction per call so the task can serve the queue between steps.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "dali_bus.h"
#include "dali_bus_priv.h"
#include "gw_events.h"

static const char *TAG = "bus";

/* Opcodes absent from the driver's header; values from IEC 62386-102. */
#define OPCODE_IDENTIFY_DEVICE 0x25U
#define OPCODE_GO_TO_LAST_LEVEL 0x0AU
#define OPCODE_QUERY_NEXT_DT 0xA7U

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_action(gw_result_t *res, const char *action)
{
    strlcpy(res->action, action, sizeof(res->action));
}

static void fail(gw_result_t *res, gw_err_t err, const char *message)
{
    res->ok = false;
    res->error = err;
    strlcpy(res->message, message, sizeof(res->message));
}

/* --- registry updates ------------------------------------------------------------------------ */

static void decode_status(gw_gear_status_t *out, int reply)
{
    memset(out, 0, sizeof(*out));
    if (!DALI_RESULT_IS_VALID(reply)) {
        return;
    }
    uint8_t raw = (uint8_t)reply;
    out->raw = raw;
    out->valid = true;
    out->gear_failure = (raw & 0x01) != 0;
    out->lamp_failure = (raw & 0x02) != 0;
    out->lamp_on = (raw & 0x04) != 0;
    out->limit_error = (raw & 0x08) != 0;
    out->fade_running = (raw & 0x10) != 0;
    out->reset_state = (raw & 0x20) != 0;
    out->missing_short_address = (raw & 0x40) != 0;
    out->power_failure = (raw & 0x80) != 0;
}

/** Any answered transaction proves a PSU is on the bus, which is what "powered" means here. */
static void note_reply(bus_ctx_t *ctx, int reply)
{
    if (DALI_RESULT_IS_VALID(reply) && !ctx->powered) {
        ctx->powered = true;
        bus_notify_state(ctx);
    }
}

static void cache_level(bus_ctx_t *ctx, uint8_t addr, int reply)
{
    if (addr >= GW_MAX_GEARS || !DALI_RESULT_IS_VALID(reply)) {
        return;
    }
    gw_gear_t *g = &ctx->gears[addr];
    if (!g->level_valid || g->level != (uint8_t)reply) {
        g->level = (uint8_t)reply;
        g->level_valid = true;
        bus_notify_gear(addr);
    }
}

/* --- short commands -------------------------------------------------------------------------- */

static gw_err_t send_level(bus_ctx_t *ctx, gw_target_t target, uint8_t level)
{
    /* is_cmd = false makes this a DAPC level frame rather than an indirect command. */
    return bus_transact(ctx, target, false, level, false, NULL);
}

static void op_set_level(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "set_level");
    if (cmd->args.set_level.has_fade_time) {
        // TODO(M3): DTR0 = fade_time then STORE DTR AS FADE TIME, before the DAPC frame.
    }
    gw_err_t err = send_level(ctx, cmd->target, cmd->args.set_level.level);
    if (err != GW_OK) {
        fail(res, err, "level frame not sent");
        return;
    }
    res->ok = true;
    if (cmd->target.type == GW_TARGET_SHORT) {
        cache_level(ctx, cmd->target.addr, cmd->args.set_level.level);
    }
}

static void op_on_off(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    bool on = cmd->kind == GW_CMD_ON;
    set_action(res, on ? "on" : "off");

    uint8_t opcode = DALI_CMD_OFF;
    if (on) {
        /*
         * GO TO LAST ACTIVE LEVEL is DALI-2 only and produces no reply, so the fallback cannot be
         * driven by the bus: it is chosen from the cached version number. Group and broadcast
         * targets cover gears of mixed versions, so they take the safe DALI-1 command.
         */
        bool dali2 = cmd->target.type == GW_TARGET_SHORT && cmd->target.addr < GW_MAX_GEARS &&
                     ctx->gears[cmd->target.addr].version_major >= 2;
        opcode = dali2 ? OPCODE_GO_TO_LAST_LEVEL : DALI_CMD_RECALL_MAX_LEVEL;
    }

    gw_err_t err = bus_transact(ctx, cmd->target, true, opcode, false, NULL);
    if (err != GW_OK) {
        fail(res, err, on ? "on command not sent" : "off command not sent");
        return;
    }
    res->ok = true;
    if (cmd->target.type == GW_TARGET_SHORT && !on) {
        cache_level(ctx, cmd->target.addr, 0);
    }
}

static void op_indirect(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "cmd");

    uint8_t opcode = 0;
    bool send_twice = false;
    if (strcmp(cmd->args.indirect.name, "go_to_scene") == 0) {
        if (cmd->args.indirect.scene >= GW_MAX_SCENES) {
            fail(res, GW_ERR_INVALID_ARG, "scene must be 0..15");
            return;
        }
        opcode = (uint8_t)(DALI_CMD_GO_TO_SCENE_0 + cmd->args.indirect.scene);
    } else if (!bus_indirect_lookup(cmd->args.indirect.name, &opcode, &send_twice)) {
        fail(res, GW_ERR_INVALID_ARG, "unknown command name");
        return;
    }

    gw_err_t err = bus_transact(ctx, cmd->target, true, opcode, send_twice, NULL);
    if (err != GW_OK) {
        fail(res, err, "command not sent");
        return;
    }
    res->ok = true;
}

static void op_query(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "query");

    int16_t opcode = cmd->args.query.opcode;
    if (opcode < 0) {
        opcode = dali_bus_query_opcode(cmd->args.query.name);
    }
    if (opcode < 0 || opcode > 0xFF) {
        fail(res, GW_ERR_INVALID_ARG, "unknown query name and no opcode");
        return;
    }

    int reply = DALI_RESULT_NO_REPLY;
    gw_err_t err = bus_transact(ctx, cmd->target, true, (uint8_t)opcode, false, &reply);
    if (err != GW_OK) {
        fail(res, err, "query not sent");
        return;
    }
    note_reply(ctx, reply);

    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data == NULL) {
        fail(res, GW_ERR_INTERNAL, "out of memory");
        return;
    }
    if (DALI_RESULT_IS_VALID(reply)) {
        cJSON_AddNumberToObject(res->data, "reply", reply);
    } else {
        cJSON_AddNullToObject(res->data, "reply");
    }
    cJSON_AddNumberToObject(res->data, "opcode", opcode);

    if (cmd->target.type == GW_TARGET_SHORT && opcode == DALI_CMD_QUERY_ACTUAL_LEVEL) {
        cache_level(ctx, cmd->target.addr, reply);
    }
}

static void op_raw(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "raw");
    uint8_t bits = cmd->args.raw.bits;
    if (bits != 16 && bits != 24) {
        fail(res, GW_ERR_INVALID_ARG, "frame must be 16 or 24 bits");
        return;
    }
    if (ctx->master == NULL) {
        fail(res, GW_ERR_INTERNAL, "bus driver not initialised");
        return;
    }

    uint8_t buf[3];
    size_t len = bits / 8;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(cmd->args.raw.frame >> (8 * (len - 1 - i)));
    }

    /* Unrestricted by design (SPEC 7.4): the raw console is a diagnostic tool, so it is logged. */
    ESP_LOGW(TAG, "raw frame %0*" PRIX32 " (%u bits)%s", (int)(len * 2), cmd->args.raw.frame, bits,
             cmd->args.raw.send_twice ? " twice" : "");

    int reply = DALI_RESULT_NO_REPLY;
    esp_err_t err = dali_master_do_raw_transaction(ctx->master, buf, len, cmd->args.raw.send_twice,
                                                   BUS_TX_TIMEOUT_MS, &reply);
    gw_event_post(GW_EVENT_BUS_ACTIVITY, NULL, 0);
    if (err != ESP_OK) {
        fail(res, gw_api_err_from_esp(err), "frame not sent");
        return;
    }
    note_reply(ctx, reply);

    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data == NULL) {
        fail(res, GW_ERR_INTERNAL, "out of memory");
        return;
    }
    if (cmd->args.raw.expect_reply && DALI_RESULT_IS_VALID(reply)) {
        cJSON_AddNumberToObject(res->data, "reply", reply);
    } else {
        cJSON_AddNullToObject(res->data, "reply");
    }
}

/**
 * The bus has no "is it powered" register: the only evidence is whether anything answers. A
 * broadcast QUERY CONTROL GEAR draws a reply (or a collision, which the driver reports as a
 * mangled byte) from any gear present on a powered bus.
 */
static void op_bus_check(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    (void)cmd;
    set_action(res, "bus_check");

    const gw_target_t broadcast = {.type = GW_TARGET_BROADCAST, .addr = 0};
    int reply = DALI_RESULT_NO_REPLY;
    gw_err_t err = bus_transact(ctx, broadcast, true, DALI_CMD_QUERY_CONTROL_GEAR, false, &reply);
    if (err != GW_OK) {
        fail(res, err, "could not drive the bus");
        ctx->powered = false;
        bus_notify_state(ctx);
        return;
    }

    bool answered = DALI_RESULT_IS_VALID(reply);
    if (ctx->powered != answered) {
        ctx->powered = answered;
        bus_notify_state(ctx);
    }

    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data == NULL) {
        fail(res, GW_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddBoolToObject(res->data, "powered", answered);
    cJSON_AddBoolToObject(res->data, "any_reply", answered);
}

static void op_identify(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "identify");
    /* TODO(M3): DALI-1 gears ignore this; fall back to a blink sequence run as a long operation
     * so the bus is not held for identify_blink_ms at a time. */
    gw_err_t err = bus_transact(ctx, cmd->target, true, OPCODE_IDENTIFY_DEVICE, true, NULL);
    if (err != GW_OK) {
        fail(res, err, "identify not sent");
        return;
    }
    res->ok = true;
}

void bus_exec_short(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    int64_t started = now_ms();
    res->id = cmd->id;
    res->has_target = true;
    res->target = cmd->target;

    if (ctx->master == NULL) {
        set_action(res, "error");
        fail(res, GW_ERR_INTERNAL, "bus driver not initialised");
        return;
    }

    switch (cmd->kind) {
        case GW_CMD_SET_LEVEL:
            op_set_level(ctx, cmd, res);
            break;
        case GW_CMD_ON:
        case GW_CMD_OFF:
            op_on_off(ctx, cmd, res);
            break;
        case GW_CMD_INDIRECT:
            op_indirect(ctx, cmd, res);
            break;
        case GW_CMD_QUERY:
            op_query(ctx, cmd, res);
            break;
        case GW_CMD_RAW:
            op_raw(ctx, cmd, res);
            break;
        case GW_CMD_BUS_CHECK:
            op_bus_check(ctx, cmd, res);
            break;
        case GW_CMD_IDENTIFY:
            op_identify(ctx, cmd, res);
            break;
        case GW_CMD_CONFIGURE:
        case GW_CMD_SET_SHORT_ADDRESS:
        case GW_CMD_REMOVE_SHORT_ADDRESS:
            // TODO(M3): DTR0 write, STORE DTR AS ... send-twice, then read back and verify.
            set_action(res, "unsupported");
            fail(res, GW_ERR_UNSUPPORTED, "commissioning lands in M3");
            break;
        case GW_CMD_COLOR:
            // TODO(M4): dali_master_set_color() once the registry knows which gears are DT8.
            set_action(res, "color");
            fail(res, GW_ERR_UNSUPPORTED, "DT8 colour lands in M4");
            break;
        default:
            set_action(res, "error");
            fail(res, GW_ERR_INVALID_ARG, "unknown command");
            break;
    }
    res->duration_ms = (uint32_t)(now_ms() - started);
}

/* --- long operations --------------------------------------------------------------------------
 *
 * One DALI transaction per bus_long_step() call. A 64-address scan is about 4 s of bus time, so
 * holding the task for its duration would make the UI feel dead and make cancel impossible.
 */

enum {
    SCAN_PRESENT = 0,
    SCAN_STATUS,
    SCAN_LEVEL,
    SCAN_DEVICE_TYPE,
    SCAN_VERSION,
    SCAN_SUBSTEP_COUNT,
};

void bus_long_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    res->id = cmd->id;
    memset(&ctx->op, 0, sizeof(ctx->op));
    ctx->op.kind = cmd->kind;
    ctx->op.id = cmd->id;
    ctx->op.origin = cmd->origin;
    ctx->op.started_us = esp_timer_get_time();

    switch (cmd->kind) {
        case GW_CMD_SCAN:
            set_action(res, "scan");
            ctx->op.total = GW_MAX_GEARS;
            ctx->op.deep = cmd->args.scan.deep;
            break;
        case GW_CMD_POLL_ALL:
            set_action(res, "poll_all");
            ctx->op.total = GW_MAX_GEARS;
            break;
        case GW_CMD_COMMISSION:
            // TODO(M3): see docs/adr for why this is not the driver's blocking dali_commission().
            set_action(res, "commission");
            ctx->op.kind = GW_CMD_NONE;
            fail(res, GW_ERR_UNSUPPORTED, "commissioning lands in M3");
            return;
        default:
            ctx->op.kind = GW_CMD_NONE;
            fail(res, GW_ERR_INVALID_ARG, "not a long operation");
            return;
    }

    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data != NULL) {
        cJSON_AddBoolToObject(res->data, "started", true);
    }
}

static void emit_progress(bus_ctx_t *ctx, const char *operation)
{
    int64_t now = esp_timer_get_time();
    if (now - ctx->op.last_progress_us < BUS_PROGRESS_INTERVAL_MS * 1000) {
        return;
    }
    ctx->op.last_progress_us = now;

    gw_event_progress_t ev = {
        .done = ctx->op.cursor,
        .total = ctx->op.total,
        .found = ctx->op.found,
    };
    strlcpy(ev.operation, operation, sizeof(ev.operation));
    gw_event_post(GW_EVENT_PROGRESS, &ev, sizeof(ev));
}

static void finish(bus_ctx_t *ctx, const char *action, gw_result_t *res, bool *done)
{
    *done = true;
    res->id = ctx->op.id;
    set_action(res, action);
    res->duration_ms = (uint32_t)((esp_timer_get_time() - ctx->op.started_us) / 1000);

    if (ctx->op.cancel) {
        fail(res, GW_ERR_CANCELLED, "cancelled");
        return;
    }
    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data != NULL) {
        cJSON_AddNumberToObject(res->data, "found", ctx->op.found);
        cJSON_AddNumberToObject(res->data, "scanned", ctx->op.cursor);
    }
}

static void step_scan(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    if (ctx->op.cancel || ctx->op.cursor >= GW_MAX_GEARS) {
        if (!ctx->op.cancel) {
            ctx->last_scan = time(NULL);
        }
        finish(ctx, "scan", res, done);
        return;
    }

    uint8_t addr = (uint8_t)ctx->op.cursor;
    gw_gear_t *gear = &ctx->gears[addr];
    const gw_target_t target = {.type = GW_TARGET_SHORT, .addr = addr};
    int reply = DALI_RESULT_NO_REPLY;

    switch (ctx->op.substep) {
        case SCAN_PRESENT: {
            bus_transact(ctx, target, true, DALI_CMD_QUERY_CONTROL_GEAR, false, &reply);
            note_reply(ctx, reply);
            bool present = DALI_RESULT_IS_VALID(reply);
            if (present != gear->present) {
                gear->present = present;
                bus_notify_gear(addr);
            }
            if (!present) {
                /* An absent gear keeps its name and last known values (SPEC 7.4). */
                ctx->op.cursor++;
                ctx->op.substep = SCAN_PRESENT;
                break;
            }
            /* TODO(M4): unix seconds need SNTP; before that this is uptime-relative. */
            gear->last_seen = time(NULL);
            ctx->op.found++;
            ctx->op.substep = SCAN_STATUS;
            break;
        }
        case SCAN_STATUS:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_STATUS, false, &reply);
            decode_status(&gear->status, reply);
            ctx->op.substep = SCAN_LEVEL;
            break;
        case SCAN_LEVEL:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_ACTUAL_LEVEL, false, &reply);
            cache_level(ctx, addr, reply);
            ctx->op.substep = SCAN_DEVICE_TYPE;
            break;
        case SCAN_DEVICE_TYPE:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_DEVICE_TYPE, false, &reply);
            gear->device_type_count = 0;
            if (DALI_RESULT_IS_VALID(reply) && reply != 0xFF) {
                gear->device_types[0] = (uint8_t)reply;
                gear->device_type_count = 1;
                gear->dt8.supported = (reply == 8);
            }
            // TODO(M4): reply == 0xFF means several types; loop QUERY NEXT DEVICE TYPE.
            ctx->op.substep = SCAN_VERSION;
            break;
        case SCAN_VERSION:
        default:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_VERSION, false, &reply);
            if (DALI_RESULT_IS_VALID(reply)) {
                /* Part 102 encodes the version as major.minor in one byte from IEC 62386-102 ed2.
                 */
                gear->version_major = (uint8_t)(reply >> 2);
                gear->version_minor = (uint8_t)(reply & 0x03);
            }
            bus_notify_gear(addr);
            ctx->op.cursor++;
            ctx->op.substep = SCAN_PRESENT;
            break;
    }

    emit_progress(ctx, "scan");
}

static void step_poll_all(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    while (ctx->op.cursor < GW_MAX_GEARS && !ctx->gears[ctx->op.cursor].present) {
        ctx->op.cursor++;
    }
    if (ctx->op.cancel || ctx->op.cursor >= GW_MAX_GEARS) {
        finish(ctx, "poll_all", res, done);
        return;
    }

    uint8_t addr = (uint8_t)ctx->op.cursor;
    const gw_target_t target = {.type = GW_TARGET_SHORT, .addr = addr};
    int reply = DALI_RESULT_NO_REPLY;

    if (ctx->op.substep == 0) {
        bus_transact(ctx, target, true, DALI_CMD_QUERY_ACTUAL_LEVEL, false, &reply);
        cache_level(ctx, addr, reply);
        ctx->op.substep = 1;
    } else {
        gw_gear_status_t before = ctx->gears[addr].status;
        bus_transact(ctx, target, true, DALI_CMD_QUERY_STATUS, false, &reply);
        decode_status(&ctx->gears[addr].status, reply);
        /* Publish only on change (SPEC 7.4): a 30 s poll must not spam retained topics. */
        if (memcmp(&before, &ctx->gears[addr].status, sizeof(before)) != 0) {
            bus_notify_gear(addr);
        }
        ctx->op.substep = 0;
        ctx->op.cursor++;
        ctx->op.found++;
    }
    emit_progress(ctx, "poll_all");
}

void bus_long_step(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    *done = false;
    switch (ctx->op.kind) {
        case GW_CMD_SCAN:
            step_scan(ctx, done, res);
            break;
        case GW_CMD_POLL_ALL:
            step_poll_all(ctx, done, res);
            break;
        default:
            finish(ctx, "unknown", res, done);
            break;
    }
}

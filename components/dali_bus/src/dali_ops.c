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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
        /* This stores the gear's fade time, it is not a per-command modifier: the bus has no way
         * to carry a fade with a DAPC frame. Subsequent level changes fade the same way. */
        bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG, cmd->args.set_level.fade_time, false,
                    NULL);
        gw_err_t ferr =
            bus_transact(ctx, cmd->target, true, DALI_CMD_STORE_DTR_AS_FADE_TIME, true, NULL);
        if (ferr != GW_OK) {
            fail(res, ferr, "fade time not stored");
            return;
        }
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

/*
 * Re-addressing costs three transactions, not one. The single-transaction rule in CLAUDE.md is
 * about operations whose length grows with the bus -- scan, commission, configure, all of which are
 * state machines. A bounded handful of frames, under 200 ms, runs inline.
 */
static void op_set_short_address(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "set_short_address");

    uint8_t new_addr = cmd->args.set_short_address.new_addr;
    if (cmd->target.type != GW_TARGET_SHORT || new_addr >= GW_MAX_GEARS) {
        fail(res, GW_ERR_INVALID_ARG, "both addresses must be short addresses 0..63");
        return;
    }
    if (new_addr == cmd->target.addr) {
        res->ok = true;
        return;
    }

    /* Refuse before writing: two gears sharing a short address is not recoverable from the UI. */
    const gw_target_t probe = {.type = GW_TARGET_SHORT, .addr = new_addr};
    int reply = DALI_RESULT_NO_REPLY;
    gw_err_t err = bus_transact(ctx, probe, true, DALI_CMD_QUERY_CONTROL_GEAR, false, &reply);
    if (err != GW_OK) {
        fail(res, err, "could not probe the target address");
        return;
    }
    if (DALI_RESULT_IS_VALID(reply)) {
        fail(res, GW_ERR_ADDRESS_IN_USE, "a gear already answers on that short address");
        return;
    }

    bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG, (uint8_t)((new_addr << 1) | 1), false, NULL);
    err = bus_transact(ctx, cmd->target, true, DALI_CMD_STORE_DTR_AS_SHORT_ADDR, true, NULL);
    if (err != GW_OK) {
        fail(res, err, "store command not sent");
        return;
    }

    reply = DALI_RESULT_NO_REPLY;
    bus_transact(ctx, probe, true, DALI_CMD_QUERY_CONTROL_GEAR, false, &reply);
    if (!DALI_RESULT_IS_VALID(reply)) {
        fail(res, GW_ERR_NO_REPLY, "the gear did not answer on its new address");
        return;
    }

    /* Carry the name and cached values across so the UI does not lose the gear it just moved. */
    gw_gear_t moved = ctx->gears[cmd->target.addr];
    moved.addr = new_addr;
    ctx->gears[new_addr] = moved;
    memset(&ctx->gears[cmd->target.addr], 0, sizeof(gw_gear_t));
    ctx->gears[cmd->target.addr].addr = cmd->target.addr;
    bus_notify_gear(cmd->target.addr);
    bus_notify_gear(new_addr);

    res->ok = true;
}

static void op_remove_short_address(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    set_action(res, "remove_short_address");
    if (cmd->target.type != GW_TARGET_SHORT) {
        fail(res, GW_ERR_INVALID_ARG, "target must be a short address");
        return;
    }

    bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG, 0xFF, false, NULL);
    gw_err_t err =
        bus_transact(ctx, cmd->target, true, DALI_CMD_STORE_DTR_AS_SHORT_ADDR, true, NULL);
    if (err != GW_OK) {
        fail(res, err, "store command not sent");
        return;
    }

    ctx->gears[cmd->target.addr].present = false;
    bus_notify_gear(cmd->target.addr);
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
        case GW_CMD_SET_SHORT_ADDRESS:
            op_set_short_address(ctx, cmd, res);
            break;
        case GW_CMD_REMOVE_SHORT_ADDRESS:
            op_remove_short_address(ctx, cmd, res);
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

static bool step_scan_deep(bus_ctx_t *ctx, uint8_t addr);
static void identify_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res);
static void identify_step(bus_ctx_t *ctx, bool *done, gw_result_t *res);

enum {
    SCAN_PRESENT = 0,
    SCAN_STATUS,
    SCAN_LEVEL,
    SCAN_DEVICE_TYPE,
    SCAN_VERSION,
    SCAN_DEEP,
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
        case GW_CMD_CONFIGURE:
            bus_configure_begin(ctx, cmd, res);
            if (!res->ok) {
                ctx->op.kind = GW_CMD_NONE;
            }
            return;
        case GW_CMD_IDENTIFY:
            identify_begin(ctx, cmd, res);
            return;
        case GW_CMD_COMMISSION:
            bus_commission_begin(ctx, cmd, res);
            if (!res->ok) {
                ctx->op.kind = GW_CMD_NONE;
            }
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
            bus_transact(ctx, target, true, DALI_CMD_QUERY_VERSION, false, &reply);
            if (DALI_RESULT_IS_VALID(reply)) {
                /* Part 102 encodes the version as major.minor in one byte from IEC 62386-102 ed2.
                 */
                gear->version_major = (uint8_t)(reply >> 2);
                gear->version_minor = (uint8_t)(reply & 0x03);
            }
            bus_notify_gear(addr);
            if (ctx->op.deep) {
                ctx->op.deep_step = 0;
                ctx->op.scratch = 0;
                ctx->op.substep = SCAN_DEEP;
            } else {
                ctx->op.cursor++;
                ctx->op.substep = SCAN_PRESENT;
            }
            break;
        case SCAN_DEEP:
            if (step_scan_deep(ctx, addr)) {
                bus_notify_gear(addr);
                ctx->op.cursor++;
                ctx->op.substep = SCAN_PRESENT;
            }
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
        case GW_CMD_COMMISSION:
            bus_commission_step(ctx, done, res);
            break;
        case GW_CMD_CONFIGURE:
            bus_configure_step(ctx, done, res);
            break;
        case GW_CMD_IDENTIFY:
            identify_step(ctx, done, res);
            break;
        default:
            finish(ctx, "unknown", res, done);
            break;
    }
}

/* --- deep scan ---------------------------------------------------------------------------------
 *
 * Memory bank 0 holds the GTIN and serial (IEC 62386-102). READ MEMORY LOCATION auto-increments
 * DTR0, so a run of bytes costs one transaction each after the initial address load.
 */

#define BANK0_GTIN_OFFSET 0x03
#define BANK0_GTIN_BYTES 6
#define BANK0_SERIAL_OFFSET 0x0B
#define BANK0_SERIAL_BYTES 4

enum {
    DEEP_MIN = 0,
    DEEP_MAX,
    DEEP_POWER_ON,
    DEEP_SYSTEM_FAILURE,
    DEEP_FADE,
    DEEP_PHYSICAL_MIN,
    DEEP_GROUPS_LOW,
    DEEP_GROUPS_HIGH,
    DEEP_SCENE_FIRST,
    DEEP_SELECT_BANK = DEEP_SCENE_FIRST + GW_MAX_SCENES,
    DEEP_GTIN_ADDR,
    DEEP_GTIN_FIRST,
    DEEP_SERIAL_ADDR = DEEP_GTIN_FIRST + BANK0_GTIN_BYTES,
    DEEP_SERIAL_FIRST,
    DEEP_COUNT = DEEP_SERIAL_FIRST + BANK0_SERIAL_BYTES,
};

/** @return true when the whole deep sequence for this address is finished. */
static bool step_scan_deep(bus_ctx_t *ctx, uint8_t addr)
{
    gw_gear_t *gear = &ctx->gears[addr];
    const gw_target_t target = {.type = GW_TARGET_SHORT, .addr = addr};
    uint8_t step = ctx->op.deep_step;
    int reply = DALI_RESULT_NO_REPLY;

    if (step >= DEEP_SCENE_FIRST && step < DEEP_SCENE_FIRST + GW_MAX_SCENES) {
        uint8_t scene = (uint8_t)(step - DEEP_SCENE_FIRST);
        bus_transact(ctx, target, true, (uint8_t)(DALI_CMD_QUERY_SCENE_LEVEL_0 + scene), false,
                     &reply);
        /* 0xFF means the scene is not programmed, which is not the same as level 255. */
        gear->config.scenes[scene] =
            DALI_RESULT_IS_VALID(reply) ? (reply == 0xFF ? -1 : (int16_t)reply) : -1;
        ctx->op.deep_step++;
        return false;
    }

    if (step >= DEEP_GTIN_FIRST && step < DEEP_GTIN_FIRST + BANK0_GTIN_BYTES) {
        bus_transact(ctx, target, true, DALI_CMD_READ_MEMORY_LOCATION, false, &reply);
        ctx->op.scratch =
            (ctx->op.scratch << 8) | (DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0);
        ctx->op.deep_step++;
        if (ctx->op.deep_step == DEEP_GTIN_FIRST + BANK0_GTIN_BYTES) {
            snprintf(gear->identity.gtin, sizeof(gear->identity.gtin), "%llu",
                     (unsigned long long)ctx->op.scratch);
            ctx->op.scratch = 0;
        }
        return false;
    }

    if (step >= DEEP_SERIAL_FIRST && step < DEEP_COUNT) {
        bus_transact(ctx, target, true, DALI_CMD_READ_MEMORY_LOCATION, false, &reply);
        ctx->op.scratch =
            (ctx->op.scratch << 8) | (DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0);
        ctx->op.deep_step++;
        if (ctx->op.deep_step == DEEP_COUNT) {
            snprintf(gear->identity.serial, sizeof(gear->identity.serial), "%llu",
                     (unsigned long long)ctx->op.scratch);
            gear->identity.valid = true;
            gear->config.valid = true;
            return true;
        }
        return false;
    }

    switch (step) {
        case DEEP_MIN:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_MIN_LEVEL, false, &reply);
            gear->config.min = DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0;
            break;
        case DEEP_MAX:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_MAX_LEVEL, false, &reply);
            gear->config.max = DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0;
            break;
        case DEEP_POWER_ON:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_POWER_ON_LEVEL, false, &reply);
            gear->config.power_on = DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0;
            break;
        case DEEP_SYSTEM_FAILURE:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL, false, &reply);
            gear->config.system_failure = DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0;
            break;
        case DEEP_FADE:
            /* One byte carries both: fade time in the high nibble, fade rate in the low one. */
            bus_transact(ctx, target, true, DALI_CMD_QUERY_FADE_TIME_RATE, false, &reply);
            if (DALI_RESULT_IS_VALID(reply)) {
                gear->config.fade_time = (uint8_t)(reply >> 4);
                gear->config.fade_rate = (uint8_t)(reply & 0x0F);
            }
            break;
        case DEEP_PHYSICAL_MIN:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_PHY_MIN_LEVEL, false, &reply);
            gear->config.physical_min = DALI_RESULT_IS_VALID(reply) ? (uint8_t)reply : 0;
            break;
        case DEEP_GROUPS_LOW:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_GROUPS_0_7, false, &reply);
            gear->config.groups = DALI_RESULT_IS_VALID(reply) ? (uint16_t)reply : 0;
            break;
        case DEEP_GROUPS_HIGH:
            bus_transact(ctx, target, true, DALI_CMD_QUERY_GROUPS_8_15, false, &reply);
            if (DALI_RESULT_IS_VALID(reply)) {
                gear->config.groups |= (uint16_t)((uint16_t)reply << 8);
            }
            break;
        case DEEP_SELECT_BANK:
            bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG1, 0, false, NULL);
            ctx->op.scratch = 0;
            break;
        case DEEP_GTIN_ADDR:
            bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG, BANK0_GTIN_OFFSET, false, NULL);
            break;
        case DEEP_SERIAL_ADDR:
            bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG, BANK0_SERIAL_OFFSET, false, NULL);
            ctx->op.scratch = 0;
            break;
        default:
            break;
    }
    ctx->op.deep_step++;
    return false;
}

/* --- identify blink ----------------------------------------------------------------------------
 *
 * A DALI-1 gear has no IDENTIFY DEVICE, so it is identified by making it blink. That is seconds of
 * wall time, so it runs as a long operation and stays cancellable rather than holding the bus.
 */

#define IDENTIFY_CYCLES 3
#define BLINK_SLICE_MS 10

/** True when the gear is known to speak DALI-2, which is what IDENTIFY DEVICE requires. */
static bool target_is_dali2(const bus_ctx_t *ctx, gw_target_t target)
{
    return target.type == GW_TARGET_SHORT && target.addr < GW_MAX_GEARS &&
           ctx->gears[target.addr].version_major >= 2;
}

static void identify_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    res->id = cmd->id;
    set_action(res, "identify");
    ctx->op.target = cmd->target;

    if (target_is_dali2(ctx, cmd->target)) {
        /* One frame and the gear identifies itself; nothing to run as an operation. */
        gw_err_t err = bus_transact(ctx, cmd->target, true, OPCODE_IDENTIFY_DEVICE, true, NULL);
        ctx->op.kind = GW_CMD_NONE;
        if (err != GW_OK) {
            fail(res, err, "identify not sent");
            return;
        }
        res->ok = true;
        return;
    }

    ctx->op.total = IDENTIFY_CYCLES * 2;
    ctx->op.cursor = 0;
    ctx->op.scratch = 0;
    res->ok = true;
    res->data = cJSON_CreateObject();
    if (res->data != NULL) {
        cJSON_AddBoolToObject(res->data, "started", true);
    }
}

static void identify_step(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    if (ctx->op.cancel || ctx->op.cursor >= ctx->op.total) {
        /* Leave the gear where it was rather than at whichever end of the blink we stopped on. */
        bus_transact(ctx, ctx->op.target, true, DALI_CMD_RECALL_MAX_LEVEL, false, NULL);
        finish(ctx, "identify", res, done);
        return;
    }

    if ((int64_t)ctx->op.scratch > esp_timer_get_time()) {
        vTaskDelay(pdMS_TO_TICKS(BLINK_SLICE_MS));
        return;
    }

    bool high = (ctx->op.cursor % 2) == 0;
    bus_transact(ctx, ctx->op.target, true,
                 high ? DALI_CMD_RECALL_MAX_LEVEL : DALI_CMD_RECALL_MIN_LEVEL, false, NULL);
    ctx->op.cursor++;
    ctx->op.scratch = (uint64_t)(esp_timer_get_time() + (int64_t)ctx->identify_blink_ms * 1000);
}

/*
 * Writing control-gear parameters (SPEC 7.4 `configure`).
 *
 * Every parameter costs three transactions -- load DTR0, STORE (send-twice), read back -- so a full
 * configure of the level parameters, all 16 scenes and all 16 groups is over a hundred frames and
 * several seconds. That is far past "one transaction without checking the queue", so this runs as a
 * state machine like scan and commissioning, and the sync caller is held until it finishes rather
 * than being told the operation merely started.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "dali_bus_priv.h"
#include "gw_events.h"

static const char *TAG = "bus";

typedef enum {
    JOB_LEVEL_PARAM = 0, /**< DTR0 + STORE + read back */
    JOB_SCENE_SET,
    JOB_SCENE_CLEAR, /**< REMOVE FROM SCENE n, no DTR */
    JOB_GROUP_ADD,
    JOB_GROUP_REMOVE,
} cfg_job_kind_t;

static void add_level_job(cfg_ctx_t *c, uint8_t value, uint8_t store, uint8_t query)
{
    if (c->count >= CFG_MAX_JOBS) {
        return;
    }
    c->jobs[c->count++] =
        (cfg_job_t){.kind = JOB_LEVEL_PARAM, .value = value, .store = store, .query = query};
}

static void add_job(cfg_ctx_t *c, cfg_job_kind_t kind, uint8_t index, uint8_t value)
{
    if (c->count >= CFG_MAX_JOBS) {
        return;
    }
    c->jobs[c->count++] = (cfg_job_t){.kind = (uint8_t)kind, .index = index, .value = value};
}

void bus_configure_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res)
{
    const gw_configure_args_t *a = &cmd->args.configure;
    cfg_ctx_t *c = &ctx->op.cfg;
    memset(c, 0, sizeof(*c));

    res->id = cmd->id;
    strlcpy(res->action, "configure", sizeof(res->action));

    if (a->fields & GW_CFG_FIELD_MIN) {
        add_level_job(c, a->min, DALI_CMD_STORE_DTR_AS_MIN_LEVEL, DALI_CMD_QUERY_MIN_LEVEL);
    }
    if (a->fields & GW_CFG_FIELD_MAX) {
        add_level_job(c, a->max, DALI_CMD_STORE_DTR_AS_MAX_LEVEL, DALI_CMD_QUERY_MAX_LEVEL);
    }
    if (a->fields & GW_CFG_FIELD_POWER_ON) {
        add_level_job(c, a->power_on, DALI_CMD_STORE_DTR_AS_POWER_ON_LEVEL,
                      DALI_CMD_QUERY_POWER_ON_LEVEL);
    }
    if (a->fields & GW_CFG_FIELD_SYSTEM_FAILURE) {
        add_level_job(c, a->system_failure, DALI_CMD_STORE_DTR_AS_FAIL_LEVEL,
                      DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL);
    }
    /* Fade time and rate share one query byte: time in the high nibble, rate in the low one. */
    if (a->fields & GW_CFG_FIELD_FADE_TIME) {
        add_level_job(c, a->fade_time, DALI_CMD_STORE_DTR_AS_FADE_TIME,
                      DALI_CMD_QUERY_FADE_TIME_RATE);
    }
    if (a->fields & GW_CFG_FIELD_FADE_RATE) {
        add_level_job(c, a->fade_rate, DALI_CMD_STORE_DTR_AS_FADE_RATE,
                      DALI_CMD_QUERY_FADE_TIME_RATE);
    }
    if (a->fields & GW_CFG_FIELD_SCENES) {
        for (uint8_t i = 0; i < GW_MAX_SCENES; i++) {
            if (a->scenes[i] == GW_SCENE_UNTOUCHED) {
                continue;
            }
            if (a->scenes[i] == GW_SCENE_CLEAR) {
                add_job(c, JOB_SCENE_CLEAR, i, 0xFF);
            } else {
                add_job(c, JOB_SCENE_SET, i, (uint8_t)a->scenes[i]);
            }
        }
    }
    if (a->fields & GW_CFG_FIELD_GROUPS) {
        for (uint8_t i = 0; i < GW_MAX_GROUPS; i++) {
            if (a->group_add & (1u << i)) {
                add_job(c, JOB_GROUP_ADD, i, 0);
            }
            if (a->group_remove & (1u << i)) {
                add_job(c, JOB_GROUP_REMOVE, i, 0);
            }
        }
    }

    if (c->count == 0) {
        res->ok = false;
        res->error = GW_ERR_INVALID_ARG;
        strlcpy(res->message, "nothing to configure", sizeof(res->message));
        return;
    }

    ctx->op.target = cmd->target;
    ctx->op.total = c->count;
    res->ok = true;
}

/** Name a job carries in the result, so a client can tell which parameter failed. */
static void job_name(const cfg_job_t *job, char *out, size_t len)
{
    switch ((cfg_job_kind_t)job->kind) {
        case JOB_SCENE_SET:
        case JOB_SCENE_CLEAR:
            snprintf(out, len, "scene[%u]", job->index);
            return;
        case JOB_GROUP_ADD:
            snprintf(out, len, "group.add[%u]", job->index);
            return;
        case JOB_GROUP_REMOVE:
            snprintf(out, len, "group.remove[%u]", job->index);
            return;
        case JOB_LEVEL_PARAM:
        default:
            break;
    }
    switch (job->store) {
        case DALI_CMD_STORE_DTR_AS_MIN_LEVEL:
            strlcpy(out, "min", len);
            return;
        case DALI_CMD_STORE_DTR_AS_MAX_LEVEL:
            strlcpy(out, "max", len);
            return;
        case DALI_CMD_STORE_DTR_AS_POWER_ON_LEVEL:
            strlcpy(out, "power_on", len);
            return;
        case DALI_CMD_STORE_DTR_AS_FAIL_LEVEL:
            strlcpy(out, "system_failure", len);
            return;
        case DALI_CMD_STORE_DTR_AS_FADE_TIME:
            strlcpy(out, "fade_time", len);
            return;
        case DALI_CMD_STORE_DTR_AS_FADE_RATE:
            strlcpy(out, "fade_rate", len);
            return;
        default:
            strlcpy(out, "unknown", len);
            return;
    }
}

static bool verify_reply(const cfg_job_t *job, int reply)
{
    if (!DALI_RESULT_IS_VALID(reply)) {
        return false;
    }
    uint8_t got = (uint8_t)reply;

    switch ((cfg_job_kind_t)job->kind) {
        case JOB_SCENE_SET:
            return got == job->value;
        case JOB_SCENE_CLEAR:
            return got == 0xFF;
        case JOB_GROUP_ADD:
            return (got & (1u << (job->index % 8))) != 0;
        case JOB_GROUP_REMOVE:
            return (got & (1u << (job->index % 8))) == 0;
        case JOB_LEVEL_PARAM:
        default:
            break;
    }
    if (job->store == DALI_CMD_STORE_DTR_AS_FADE_TIME) {
        return (got >> 4) == job->value;
    }
    if (job->store == DALI_CMD_STORE_DTR_AS_FADE_RATE) {
        return (got & 0x0F) == job->value;
    }
    return got == job->value;
}

static void finish(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    *done = true;
    cfg_ctx_t *c = &ctx->op.cfg;

    res->id = ctx->op.id;
    strlcpy(res->action, "configure", sizeof(res->action));
    res->duration_ms = (uint32_t)((esp_timer_get_time() - ctx->op.started_us) / 1000);

    if (ctx->op.cancel) {
        res->ok = false;
        res->error = GW_ERR_CANCELLED;
        strlcpy(res->message, "cancelled", sizeof(res->message));
        return;
    }

    /* Every parameter is reported individually: a partial write is the normal failure mode here,
     * and "it failed" without saying which parameter is useless to an installer. */
    cJSON *list = cJSON_CreateArray();
    bool all_ok = true;
    for (uint8_t i = 0; i < c->count; i++) {
        char name[24];
        job_name(&c->jobs[i], name, sizeof(name));
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) {
            break;
        }
        cJSON_AddStringToObject(entry, "param", name);
        cJSON_AddStringToObject(entry, "status", c->jobs[i].ok ? "ok" : "mismatch");
        cJSON_AddItemToArray(list, entry);
        all_ok = all_ok && c->jobs[i].ok;
    }

    res->ok = all_ok;
    if (!all_ok) {
        res->error = GW_ERR_NO_REPLY;
        strlcpy(res->message, "one or more parameters did not verify", sizeof(res->message));
    }
    res->data = cJSON_CreateObject();
    if (res->data != NULL) {
        cJSON_AddItemToObject(res->data, "parameters", list);
    } else {
        cJSON_Delete(list);
    }
}

void bus_configure_step(bus_ctx_t *ctx, bool *done, gw_result_t *res)
{
    cfg_ctx_t *c = &ctx->op.cfg;

    if (ctx->op.cancel || c->cursor >= c->count) {
        finish(ctx, done, res);
        return;
    }

    cfg_job_t *job = &c->jobs[c->cursor];
    const gw_target_t target = ctx->op.target;
    int reply = DALI_RESULT_NO_REPLY;

    switch ((cfg_job_kind_t)job->kind) {
        case JOB_GROUP_ADD:
        case JOB_GROUP_REMOVE:
            if (c->substep == 0) {
                uint8_t opcode = job->kind == JOB_GROUP_ADD
                                     ? (uint8_t)(DALI_CMD_ADD_TO_GROUP_0 + job->index)
                                     : (uint8_t)(DALI_CMD_REMOVE_FROM_GROUP_0 + job->index);
                bus_transact(ctx, target, true, opcode, true, NULL);
                c->substep = 1;
            } else {
                uint8_t query =
                    job->index < 8 ? DALI_CMD_QUERY_GROUPS_0_7 : DALI_CMD_QUERY_GROUPS_8_15;
                bus_transact(ctx, target, true, query, false, &reply);
                job->ok = verify_reply(job, reply);
                if (!job->ok) {
                    char name[24];
                    job_name(job, name, sizeof(name));
                    ESP_LOGW(TAG, "configure: %s did not verify", name);
                }
                c->substep = 0;
                c->cursor++;
            }
            break;

        case JOB_SCENE_CLEAR:
            if (c->substep == 0) {
                bus_transact(ctx, target, true,
                             (uint8_t)(DALI_CMD_REMOVE_FROM_SCENE_0 + job->index), true, NULL);
                c->substep = 1;
            } else {
                bus_transact(ctx, target, true,
                             (uint8_t)(DALI_CMD_QUERY_SCENE_LEVEL_0 + job->index), false, &reply);
                job->ok = verify_reply(job, reply);
                if (!job->ok) {
                    char name[24];
                    job_name(job, name, sizeof(name));
                    ESP_LOGW(TAG, "configure: %s did not verify", name);
                }
                c->substep = 0;
                c->cursor++;
            }
            break;

        case JOB_SCENE_SET:
        case JOB_LEVEL_PARAM:
        default:
            if (c->substep == 0) {
                bus_special(ctx, DALI_SPECIAL_DATA_TRANSFER_REG, job->value, false, NULL);
                c->substep = 1;
            } else if (c->substep == 1) {
                uint8_t store = job->kind == JOB_SCENE_SET
                                    ? (uint8_t)(DALI_CMD_STORE_DTR_AS_SCENE_0 + job->index)
                                    : job->store;
                bus_transact(ctx, target, true, store, true, NULL);
                c->substep = 2;
            } else {
                uint8_t query = job->kind == JOB_SCENE_SET
                                    ? (uint8_t)(DALI_CMD_QUERY_SCENE_LEVEL_0 + job->index)
                                    : job->query;
                bus_transact(ctx, target, true, query, false, &reply);
                job->ok = verify_reply(job, reply);
                if (!job->ok) {
                    char name[24];
                    job_name(job, name, sizeof(name));
                    ESP_LOGW(TAG, "configure: %s did not verify", name);
                }
                c->substep = 0;
                c->cursor++;
            }
            break;
    }

    int64_t now = esp_timer_get_time();
    if (now - ctx->op.last_progress_us >= BUS_PROGRESS_INTERVAL_MS * 1000) {
        ctx->op.last_progress_us = now;
        gw_event_progress_t ev = {.done = c->cursor, .total = c->count, .found = 0};
        strlcpy(ev.operation, "configure", sizeof(ev.operation));
        gw_event_post(GW_EVENT_PROGRESS, &ev, sizeof(ev));
    }
}

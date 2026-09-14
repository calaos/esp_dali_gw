/** Internals shared between the bus task, the operations and the name tables. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dali.h"
#include "gw_api.h"

/** Per-frame TX timeout handed to the driver. Its own default is 110 ms. */
#define BUS_TX_TIMEOUT_MS 110

/** A long operation emits progress at least this often, even when steps are slower (SPEC 7.2). */
#define BUS_PROGRESS_INTERVAL_MS 250

typedef struct {
    gw_cmd_kind_t kind; /**< GW_CMD_NONE when idle */
    uint32_t id;
    gw_origin_t origin;
    uint16_t cursor; /**< next short address to visit, or step index */
    uint8_t substep; /**< which query of the per-address sequence comes next */
    uint16_t total;
    uint16_t found;
    bool deep;
    bool cancel;
    int64_t started_us;
    int64_t last_progress_us;
} bus_long_op_t;

/** Everything the operations need from the bus task. One instance, owned by the task. */
typedef struct {
    dali_master_handle_t master;
    bool powered;
    gw_gear_t gears[GW_MAX_GEARS];
    bus_long_op_t op;
    int64_t last_scan;
} bus_ctx_t;

/* --- transaction helpers (dali_bus.c) ------------------------------------------------------- */

/**
 * @brief One Part 102 transaction, with the bus-activity event and the powered-state bookkeeping.
 *
 * @param[out] reply  backward frame byte, or -1 when the gear did not answer; may be NULL
 */
gw_err_t bus_transact(bus_ctx_t *ctx, gw_target_t target, bool is_cmd, uint8_t opcode,
                      bool send_twice, int *reply);

/** @brief A special command (DALI_ADDR_SPECIAL), e.g. DTR0 loading. */
gw_err_t bus_special(bus_ctx_t *ctx, uint8_t special, uint8_t data, bool send_twice, int *reply);

/** @brief Publish GW_EVENT_GEAR_CHANGED for @p addr. */
void bus_notify_gear(uint8_t addr);

/** @brief Publish the bus powered/busy state. */
void bus_notify_state(const bus_ctx_t *ctx);

/* --- operations (dali_ops.c) ---------------------------------------------------------------- */

/** @brief Execute a command that costs a bounded number of transactions. */
void bus_exec_short(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res);

/** @brief Begin a long operation. @p res receives the immediate `{started:true}` acknowledgement.
 */
void bus_long_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res);

/**
 * @brief Advance the running long operation by one DALI transaction.
 *
 * @param[out] done  set when the operation finished or was cancelled
 * @param[out] res   final result, valid only when @p done
 */
void bus_long_step(bus_ctx_t *ctx, bool *done, gw_result_t *res);

/* --- name tables (dali_names.c) ------------------------------------------------------------- */

/** @brief Opcode for an indirect command name, and whether it needs send-twice. */
bool bus_indirect_lookup(const char *name, uint8_t *opcode, bool *send_twice);

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

/** Where the Part 102 binary search currently is (ADR 0004). */
typedef enum {
    COMM_QUIESCENT = 0,
    COMM_TERMINATE_START,
    COMM_INITIALISE,
    COMM_RANDOMISE,
    COMM_SETTLE,
    COMM_ROUND_START,
    COMM_WRITE_SEARCH,
    COMM_COMPARE,
    COMM_PROGRAM,
    COMM_VERIFY,
    COMM_WITHDRAW,
    COMM_TERMINATE_END,
} comm_state_t;

/** One parameter write: DTR0, then the STORE command, then a read-back to verify (SPEC 7.4). */
typedef struct {
    uint8_t kind;  /**< cfg_job_kind_t */
    uint8_t index; /**< scene or group number */
    uint8_t value;
    uint8_t store; /**< STORE opcode, for the level parameters */
    uint8_t query; /**< read-back opcode */
    bool ok;
    bool verified;
} cfg_job_t;

#define CFG_MAX_JOBS 40 /**< 7 level parameters + 16 scenes + 16 groups, plus slack */

typedef struct {
    cfg_job_t jobs[CFG_MAX_JOBS];
    uint8_t count;
    uint8_t cursor;
    uint8_t substep;
} cfg_ctx_t;

typedef struct {
    comm_state_t state;
    gw_commission_mode_t mode;
    uint32_t low, high; /**< bisection bounds over the 24-bit random address */
    uint32_t search;    /**< value being probed */
    uint32_t written;   /**< what the gears currently hold, to skip unchanged bytes */
    bool written_valid;
    uint8_t byte_index; /**< which of H/M/L still needs writing this step */
    uint8_t next_addr;  /**< short address to hand out next */
    uint8_t max_devices;
    int64_t settle_until_us;
    bool round_open; /**< a device was selected and is mid-assignment */
} comm_ctx_t;

typedef struct {
    gw_cmd_kind_t kind; /**< GW_CMD_NONE when idle */
    uint32_t id;
    gw_target_t target; /**< the gear being configured; unused by scan and poll */
    gw_origin_t origin;
    uint16_t cursor;   /**< next short address to visit, or step index */
    uint8_t substep;   /**< which query of the per-address sequence comes next */
    uint8_t deep_step; /**< position in the deep per-address sequence */
    uint64_t scratch;  /**< accumulates the multi-byte memory-bank values */
    uint16_t total;
    uint16_t found;
    bool deep;
    bool cancel;
    int64_t started_us;
    int64_t last_progress_us;
    comm_ctx_t comm;
    cfg_ctx_t cfg;
    /** Sync caller waiting for the final result instead of a started ack. */
    void *reply_handle;
} bus_long_op_t;

/** Everything the operations need from the bus task. One instance, owned by the task. */
typedef struct {
    dali_master_handle_t master;
    bool powered;
    gw_gear_t gears[GW_MAX_GEARS];
    bus_long_op_t op;
    uint16_t identify_blink_ms;
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

/* --- commissioning (dali_commission.c) ------------------------------------------------------- */

/** @brief Set up the state machine. @p res receives the immediate acknowledgement. */
void bus_commission_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res);

/** @brief Advance commissioning by one transaction (or one bounded wait). */
void bus_commission_step(bus_ctx_t *ctx, bool *done, gw_result_t *res);

/* --- configure (dali_configure.c) ------------------------------------------------------------ */

/** @brief Build the job list from the command. Sets res->ok false when nothing would be written. */
void bus_configure_begin(bus_ctx_t *ctx, const gw_cmd_t *cmd, gw_result_t *res);

/** @brief Advance the configuration by one transaction. */
void bus_configure_step(bus_ctx_t *ctx, bool *done, gw_result_t *res);

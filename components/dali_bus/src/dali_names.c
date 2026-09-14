/*
 * Name-to-opcode tables. DALI opcode knowledge lives here and nowhere else: the wire schema in
 * gw_api carries command and query names as strings, which is what keeps gw_api free of the IDF
 * and host-testable.
 */
#include <string.h>

#include "dali_bus.h"
#include "dali_bus_priv.h"

/*
 * Opcodes the driver's dali_command.h does not expose. Values are from IEC 62386-102; they are
 * declared here rather than patched into the managed component so the component stays pristine.
 */
#define OPCODE_GO_TO_LAST_ACTIVE_LEVEL 0x0AU
#define OPCODE_QUERY_NEXT_DEVICE_TYPE 0xA7U
#define OPCODE_QUERY_EXTENDED_FADE 0xA8U
#define OPCODE_IDENTIFY_DEVICE 0x25U

typedef struct {
    const char *name;
    uint8_t opcode;
    bool send_twice;
} named_cmd_t;

/* Part 102 indirect commands reachable through `cmd`. Send-twice is a property of the command, not
 * of the caller, so it is decided here (IEC 62386-102 table 4). */
static const named_cmd_t k_indirect[] = {
    {"off", DALI_CMD_OFF, false},
    {"up", DALI_CMD_UP, false},
    {"down", DALI_CMD_DOWN, false},
    {"step_up", DALI_CMD_STEP_UP, false},
    {"step_down", DALI_CMD_STEP_DOWN, false},
    {"recall_max", DALI_CMD_RECALL_MAX_LEVEL, false},
    {"recall_min", DALI_CMD_RECALL_MIN_LEVEL, false},
    {"step_down_and_off", DALI_CMD_STEP_DOWN_AND_OFF, false},
    {"on_and_step_up", DALI_CMD_ON_AND_STEP_UP, false},
    {"go_to_last_active", OPCODE_GO_TO_LAST_ACTIVE_LEVEL, false},
    {"reset", DALI_CMD_RESET, true},
};

static const named_cmd_t k_queries[] = {
    {"status", DALI_CMD_QUERY_STATUS, false},
    {"control_gear_present", DALI_CMD_QUERY_CONTROL_GEAR, false},
    {"lamp_failure", DALI_CMD_QUERY_LAMP_FAILURE, false},
    {"lamp_power_on", DALI_CMD_QUERY_LAMP_POWER_ON, false},
    {"limit_error", DALI_CMD_QUERY_LIMIT_ERROR, false},
    {"reset_state", DALI_CMD_QUERY_RESET_STATE, false},
    {"missing_short_address", DALI_CMD_QUERY_MISSING_SHORT_ADDR, false},
    {"version_number", DALI_CMD_QUERY_VERSION, false},
    {"device_type", DALI_CMD_QUERY_DEVICE_TYPE, false},
    {"next_device_type", OPCODE_QUERY_NEXT_DEVICE_TYPE, false},
    {"physical_minimum", DALI_CMD_QUERY_PHY_MIN_LEVEL, false},
    {"power_failure", DALI_CMD_QUERY_POWER_FAILURE, false},
    {"content_dtr0", DALI_CMD_QUERY_CONTENT_DTR, false},
    {"content_dtr1", DALI_CMD_QUERY_CONTENT_DTR1, false},
    {"content_dtr2", DALI_CMD_QUERY_CONTENT_DTR2, false},
    {"actual_level", DALI_CMD_QUERY_ACTUAL_LEVEL, false},
    {"max_level", DALI_CMD_QUERY_MAX_LEVEL, false},
    {"min_level", DALI_CMD_QUERY_MIN_LEVEL, false},
    {"power_on_level", DALI_CMD_QUERY_POWER_ON_LEVEL, false},
    {"system_failure_level", DALI_CMD_QUERY_SYSTEM_FAILURE_LEVEL, false},
    {"fade_time_fade_rate", DALI_CMD_QUERY_FADE_TIME_RATE, false},
    {"extended_fade_time", OPCODE_QUERY_EXTENDED_FADE, false},
    {"extended_version", DALI_CMD_QUERY_EXTENDED_VERSION, false},
    {"groups_0_7", DALI_CMD_QUERY_GROUPS_0_7, false},
    {"groups_8_15", DALI_CMD_QUERY_GROUPS_8_15, false},
    {"random_address_h", DALI_CMD_QUERY_RANDOM_ADDR_H, false},
    {"random_address_m", DALI_CMD_QUERY_RANDOM_ADDR_M, false},
    {"random_address_l", DALI_CMD_QUERY_RANDOM_ADDR_L, false},
};

static const named_cmd_t *find(const named_cmd_t *table, size_t n, const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

bool bus_indirect_lookup(const char *name, uint8_t *opcode, bool *send_twice)
{
    const named_cmd_t *c = find(k_indirect, sizeof(k_indirect) / sizeof(k_indirect[0]), name);
    if (c == NULL) {
        return false;
    }
    if (opcode != NULL) {
        *opcode = c->opcode;
    }
    if (send_twice != NULL) {
        *send_twice = c->send_twice;
    }
    return true;
}

bool dali_bus_indirect_name_valid(const char *name)
{
    /* go_to_scene:n is parsed by gw_api into a scene index, so the bare name is accepted too. */
    if (name != NULL && strcmp(name, "go_to_scene") == 0) {
        return true;
    }
    return bus_indirect_lookup(name, NULL, NULL);
}

int16_t dali_bus_query_opcode(const char *name)
{
    const named_cmd_t *c = find(k_queries, sizeof(k_queries) / sizeof(k_queries[0]), name);
    return c != NULL ? (int16_t)c->opcode : -1;
}

/**
 * @file gw_api.h
 * @brief Shared model types and the single JSON schema used by both MQTT and HTTP (SPEC 8, 9).
 *
 * Both adapters (de)serialize through this component so their payloads are byte-identical. No
 * adapter may hand-build JSON. Nothing here knows about DALI opcodes, FreeRTOS or the network:
 * command names and query names travel as strings and are resolved in dali_bus, which keeps this
 * component pure logic and host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_GEARS 64
#define GW_MAX_GROUPS 16
#define GW_MAX_SCENES 16
#define GW_NAME_LEN APP_CONFIG_NAME_LEN
#define GW_CMDNAME_LEN 24 /**< "step_down_and_off", "actual_level", ... */

/** Closed error set (SPEC 7.5). Anything reported to a client is one of these. */
typedef enum {
    GW_OK = 0,
    GW_ERR_INVALID_ARG,
    GW_ERR_BUS_BUSY,
    GW_ERR_BUS_UNPOWERED,
    GW_ERR_NO_REPLY,
    GW_ERR_TX_FAILED,
    GW_ERR_TIMEOUT,
    GW_ERR_NOT_PRESENT,
    GW_ERR_ADDRESS_IN_USE,
    GW_ERR_UNSUPPORTED,
    GW_ERR_CANCELLED,
    GW_ERR_INTERNAL,
} gw_err_t;

typedef enum {
    GW_TARGET_SHORT = 0, /**< addr = short address 0..63 */
    GW_TARGET_GROUP,     /**< addr = group 0..15 */
    GW_TARGET_BROADCAST, /**< addr ignored */
} gw_target_type_t;

typedef struct {
    gw_target_type_t type;
    uint8_t addr;
} gw_target_t;

/** Where a command came from, for logging and future ACLs. */
typedef enum {
    GW_ORIGIN_INTERNAL = 0,
    GW_ORIGIN_MQTT,
    GW_ORIGIN_HTTP,
} gw_origin_t;

typedef enum {
    GW_CMD_NONE = 0,
    GW_CMD_SET_LEVEL,
    GW_CMD_ON,
    GW_CMD_OFF,
    GW_CMD_INDIRECT, /**< up, down, step_up, recall_max, go_to_scene:n, reset, ... */
    GW_CMD_COLOR,
    GW_CMD_QUERY,
    GW_CMD_CONFIGURE,
    GW_CMD_RAW,
    GW_CMD_IDENTIFY,
    GW_CMD_SET_SHORT_ADDRESS,
    GW_CMD_REMOVE_SHORT_ADDRESS,
    GW_CMD_SCAN,
    GW_CMD_COMMISSION,
    GW_CMD_POLL_ALL,
    GW_CMD_BUS_CHECK,
    GW_CMD_CANCEL,
} gw_cmd_kind_t;

/** Which fields of gw_configure_args_t carry a value. */
typedef enum {
    GW_CFG_FIELD_MIN = 1u << 0,
    GW_CFG_FIELD_MAX = 1u << 1,
    GW_CFG_FIELD_POWER_ON = 1u << 2,
    GW_CFG_FIELD_SYSTEM_FAILURE = 1u << 3,
    GW_CFG_FIELD_FADE_TIME = 1u << 4,
    GW_CFG_FIELD_FADE_RATE = 1u << 5,
    GW_CFG_FIELD_EXT_FADE_TIME = 1u << 6,
    GW_CFG_FIELD_SCENES = 1u << 7,
    GW_CFG_FIELD_GROUPS = 1u << 8,
} gw_cfg_field_t;

#define GW_SCENE_UNTOUCHED (-2) /**< leave this scene alone */
#define GW_SCENE_CLEAR (-1)     /**< program 0xFF: scene not used */

typedef struct {
    uint32_t fields; /**< bitmask of gw_cfg_field_t */
    uint8_t min, max, power_on, system_failure, fade_time, fade_rate, ext_fade_time;
    int16_t scenes[GW_MAX_SCENES]; /**< GW_SCENE_UNTOUCHED, GW_SCENE_CLEAR, or 0..254 */
    uint16_t group_add;            /**< bit n: join group n */
    uint16_t group_remove;         /**< bit n: leave group n */
} gw_configure_args_t;

typedef enum {
    GW_COLOR_NONE = 0,
    GW_COLOR_MIREK,
    GW_COLOR_RGB,
    GW_COLOR_RGBWAF,
} gw_color_kind_t;

typedef struct {
    gw_color_kind_t kind;
    uint16_t mirek;
    uint8_t channels[6]; /**< r,g,b,w,a,f — only the first 3 used for GW_COLOR_RGB */
} gw_color_args_t;

typedef enum {
    GW_COMMISSION_UNADDRESSED = 0, /**< only gears without a short address */
    GW_COMMISSION_ALL,             /**< re-address everything; needs confirm */
} gw_commission_mode_t;

typedef struct {
    uint32_t id; /**< correlation id echoed back to the client */
    gw_cmd_kind_t kind;
    gw_target_t target;
    gw_origin_t origin;
    /** Served ahead of the next step of a running long operation (SPEC 7.2). */
    bool urgent;
    union {
        struct {
            uint8_t level; /**< 0..254 */
            bool has_fade_time;
            uint8_t fade_time;
        } set_level;
        struct {
            char name[GW_CMDNAME_LEN]; /**< indirect command name */
            uint8_t scene;             /**< for go_to_scene */
        } indirect;
        gw_color_args_t color;
        struct {
            char name[GW_CMDNAME_LEN]; /**< query name; empty when opcode is used */
            int16_t opcode;            /**< -1 when name is used */
        } query;
        gw_configure_args_t configure;
        struct {
            uint32_t frame; /**< 16- or 24-bit frame, right-aligned */
            uint8_t bits;   /**< 16 or 24 */
            bool send_twice;
            bool expect_reply;
        } raw;
        struct {
            bool deep;
        } scan;
        struct {
            gw_commission_mode_t mode;
            bool confirm;
            uint8_t start_addr;
        } commission;
        struct {
            uint8_t new_addr;
        } set_short_address;
    } args;
} gw_cmd_t;

/** Decoded QUERY STATUS bits (SPEC 7.3). @c raw is always the byte as received. */
typedef struct {
    uint8_t raw;
    bool valid; /**< false when the gear never answered */
    bool gear_failure;
    bool lamp_failure;
    bool lamp_on;
    bool limit_error;
    bool fade_running;
    bool reset_state;
    bool missing_short_address;
    bool power_failure;
} gw_gear_status_t;

typedef struct {
    bool valid;
    uint8_t min, max, power_on, system_failure, fade_time, fade_rate, physical_min;
    uint16_t groups;               /**< bit n: member of group n */
    int16_t scenes[GW_MAX_SCENES]; /**< -1 = 0xFF, not programmed */
} gw_gear_config_t;

typedef struct {
    bool valid;
    char gtin[16];   /**< decimal GTIN from memory bank 0 */
    char serial[24]; /**< decimal serial from memory bank 0 */
    uint8_t bank0_version;
} gw_gear_identity_t;

typedef struct {
    bool supported;
    uint8_t caps;
    uint16_t tc_min_mirek;
    uint16_t tc_max_mirek;
} gw_gear_dt8_t;

/** One registry entry (SPEC 7.3). Absent gears keep their name and last known values. */
typedef struct {
    uint8_t addr;
    char name[GW_NAME_LEN];
    bool present;
    int64_t last_seen; /**< unix seconds; 0 = never */
    uint8_t device_types[4];
    uint8_t device_type_count;
    uint8_t version_major, version_minor;
    gw_gear_dt8_t dt8;
    uint8_t level;
    bool level_valid;
    gw_gear_status_t status;
    gw_gear_config_t config;
    gw_gear_identity_t identity;
} gw_gear_t;

typedef struct {
    bool powered;
    bool busy;
    char operation[GW_CMDNAME_LEN]; /**< empty when idle */
    bool has_progress;
    uint16_t done, total, found;
    uint16_t gear_count;
    int64_t last_scan;
} gw_bus_status_t;

/** Result envelope (SPEC 8.3). @c data is owned by the result and freed by gw_api_result_free(). */
typedef struct {
    uint32_t id;
    char action[GW_CMDNAME_LEN];
    bool ok;
    gw_err_t error;
    char message[96];
    bool has_target;
    gw_target_t target;
    uint32_t duration_ms;
    cJSON *data; /**< may be NULL */
} gw_result_t;

/** Device status and build info (SPEC 8.1 status topic, SPEC 9 /api/info). */
typedef struct {
    char state[8]; /**< "online"; the LWT publishes "offline" on the gateway's behalf */
    char fw[32];
    char idf[16];
    char ip[16];
    char mac[18];
    char hostname[GW_NAME_LEN];
    char device_id[8];
    char mode[6]; /**< "sta", "ap" or "apsta" */
    int8_t rssi;
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
    char chip[24];
    char build_date[32];
    int reset_reason;
} gw_info_t;

/** @brief The status object of SPEC 8.1: state, fw, idf, ip, rssi, uptime_s, mac. */
cJSON *gw_api_status_to_json(const gw_info_t *info);

/** @brief The status object plus build info, heap and mode, as served on /api/info (SPEC 9). */
cJSON *gw_api_info_to_json(const gw_info_t *info);

/** @brief Wire name of an error ("bus_busy"). GW_OK maps to "ok". */
const char *gw_api_err_str(gw_err_t err);

/** @brief Map an esp_err_t from the driver layer onto the closed error set. */
gw_err_t gw_api_err_from_esp(esp_err_t err);

/** @brief Release @c data of a result built by this component. Safe on a zeroed struct. */
void gw_api_result_free(gw_result_t *res);

/* --- serialization: gateway to client ------------------------------------------------------- */

/** @brief Gear object. @p deep adds the @c config and @c identity blocks. */
cJSON *gw_api_gear_to_json(const gw_gear_t *gear, bool deep);

/** @brief Compact @c {"gears":[...]} list (addr, name, present, level, on, status.raw). */
cJSON *gw_api_gears_to_json(const gw_gear_t *gears, size_t count);

cJSON *gw_api_bus_to_json(const gw_bus_status_t *bus);
cJSON *gw_api_result_to_json(const gw_result_t *res);
cJSON *gw_api_progress_to_json(const char *operation, uint16_t done, uint16_t total,
                               uint16_t found);

/**
 * @brief Configuration document (SPEC 6).
 *
 * @param include_secrets  true only for the authenticated export path; every other caller passes
 *                         false and gets APP_CONFIG_SECRET_MASK in place of each secret.
 */
cJSON *gw_api_config_to_json(const app_config_t *cfg, bool include_secrets);

/* --- deserialization: client to gateway ----------------------------------------------------- */

/**
 * @brief Merge a (possibly partial) configuration document into @p inout.
 *
 * Absent members keep their current value; a member equal to APP_CONFIG_SECRET_MASK keeps the
 * stored secret. Does not validate ranges — call app_config_validate() afterwards.
 *
 * @param[out] err_field  dotted path of the offending member on failure; may be NULL
 */
esp_err_t gw_api_config_from_json(const cJSON *root, app_config_t *inout, char *err_field,
                                  size_t err_len);

/**
 * @brief Parse one `cmd/<action>` payload into a command.
 *
 * @param action  the action name, e.g. "scan", "configure", "raw"
 * @param root    parsed payload; may be NULL for actions that take no arguments
 */
esp_err_t gw_api_cmd_from_json(const char *action, const cJSON *root, gw_cmd_t *out, char *err_msg,
                               size_t err_len);

/**
 * @brief Parse a `set` payload for an already-resolved target.
 *
 * Accepts the full JSON forms ({"level":n}, {"on":true}, {"cmd":"up"}, {"mirek":n}, ...) as well as
 * the bare forms a home-automation system tends to publish: a plain number, "ON" and "OFF". Takes
 * raw bytes rather than a cJSON tree because "ON" is not valid JSON.
 */
esp_err_t gw_api_set_from_payload(const char *payload, size_t len, gw_target_t target,
                                  gw_cmd_t *out, char *err_msg, size_t err_len);

#ifdef __cplusplus
}
#endif

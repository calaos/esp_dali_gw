/**
 * @file app_config_logic.h
 * @brief Private helpers shared by the NVS store and the host tests.
 *
 * Everything declared here is pure: no NVS, no logging, no MAC. Keeping it in its own translation
 * unit is what lets tests/host build the validation and secret rules without the IDF.
 */
#pragma once

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Resolve every APP_CONFIG_SECRET_MASK field of @p cfg from @p stored. */
void app_config_merge_secrets_from(app_config_t *cfg, const app_config_t *stored);

/** @brief Which subsystems a move from @p cur to @p next affects (SPEC 6). */
app_config_impact_t app_config_diff_impact(const app_config_t *cur, const app_config_t *next);

/**
 * @brief Bring a loaded blob up to APP_CONFIG_SCHEMA_VERSION.
 *
 * @return ESP_ERR_INVALID_VERSION when the blob cannot be interpreted, which the caller treats
 *         exactly like a corrupt one.
 */
esp_err_t app_config_migrate(app_config_t *cfg);

#ifdef __cplusplus
}
#endif

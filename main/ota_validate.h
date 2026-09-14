/** Self-validation of a freshly flashed OTA image (SPEC 12). */
#pragma once

#include "esp_err.h"

/**
 * @brief Arm the rollback guard when running a not-yet-validated image.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE an image that never marks itself valid is rolled back
 * on the next boot. A no-op when the running image is already marked valid.
 */
esp_err_t ota_validate_init(void);

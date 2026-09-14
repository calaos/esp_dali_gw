/** BOOT-button factory reset (SPEC 5.3). */
#pragma once

#include "esp_err.h"

/**
 * @brief Watch the BOOT button and erase the configuration when it is held long enough.
 *
 * The pin is a strapping pin, so it is only ever configured and read after boot.
 */
esp_err_t factory_button_init(void);

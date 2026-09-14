/* The half of app_config that talks to NVS and to the MAC stays on the target; only the device id
 * leaks into the pure logic, through app_config_defaults(). */
#include <stdio.h>

#include "app_config.h"

void app_config_device_id(char *out, size_t len)
{
    if (out != NULL && len > 0) {
        snprintf(out, len, "a1b2c3");
    }
}

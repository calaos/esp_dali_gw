/** SNTP, started once the device has an address. */
#pragma once

#include <stdbool.h>

/**
 * @brief Start SNTP and apply the configured timezone.
 *
 * The data model timestamps gear activity in unix seconds (SPEC 7.3, 8.1), which is meaningless
 * until the clock is set. Idempotent: calling it again after a reconnect is a no-op.
 */
void time_sync_start(void);

/** @brief True once the clock has actually been set, so a timestamp can be trusted. */
bool time_sync_ready(void);

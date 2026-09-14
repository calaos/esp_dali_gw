#include "esp_log.h"

#include "gw_events.h"

static const char *TAG = "gw";

ESP_EVENT_DEFINE_BASE(DALI_GW_EVENT);

esp_err_t gw_event_post(gw_event_id_t id, const void *payload, size_t size)
{
    // Zero block time: posters include the bus task and ISR-adjacent contexts, so a saturated loop
    // must drop the event rather than stall them. DEBUG, not WARN, or a busy loop spams the log.
    esp_err_t err = esp_event_post(DALI_GW_EVENT, (int32_t)id, payload, size, 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "event %d dropped: %s", (int)id, esp_err_to_name(err));
    }
    return err;
}

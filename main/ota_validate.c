#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "gw_events.h"
#include "ota_validate.h"

static const char *TAG = "gw";

/** Escape hatch from SPEC 12: an image that stayed up this long without crashing is good enough. */
#define GRACE_US (120 * 1000 * 1000)

static bool s_net_ok;
static bool s_bus_ok;
static bool s_done;
static esp_timer_handle_t s_grace;

static void mark_valid(const char *why)
{
    if (s_done) {
        return;
    }
    s_done = true;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA image marked valid (%s)", why);
    } else {
        ESP_LOGE(TAG, "could not mark image valid: %s", esp_err_to_name(err));
    }
    if (s_grace != NULL) {
        esp_timer_stop(s_grace);
    }
}

static void on_grace(void *arg)
{
    (void)arg;
    mark_valid("grace period elapsed");
}

static void on_gw_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (s_done) {
        return;
    }
    switch ((gw_event_id_t)id) {
        case GW_EVENT_NET_STATE:
            s_net_ok = ((const gw_event_net_t *)data)->state == GW_NET_STA_CONNECTED;
            break;
        case GW_EVENT_BUS_ACTIVITY:
            s_bus_ok = true;
            break;
        default:
            return;
    }
    if (s_net_ok && s_bus_ok) {
        mark_valid("network up and bus answered");
    }
}

esp_err_t ota_validate_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "running an unvalidated image from %s; rollback armed", running->label);

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(DALI_GW_EVENT, ESP_EVENT_ANY_ID, on_gw_event, NULL), TAG,
        "event register");

    const esp_timer_create_args_t args = {.callback = on_grace, .name = "ota_grace"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_grace), TAG, "timer create");
    return esp_timer_start_once(s_grace, GRACE_US);
}

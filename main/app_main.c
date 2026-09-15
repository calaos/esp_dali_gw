#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "dali_bus.h"
#include "factory_button.h"
#include "gw_events.h"
#include "http_iface.h"
#include "mqtt_iface.h"
#include "net_wifi.h"
#include "ota_validate.h"
#include "status_led.h"

static const char *TAG = "gw";

static void banner(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char id[8];
    app_config_device_id(id, sizeof(id));

    ESP_LOGI(TAG, "esp_dali_gw %s (%s %s)", app->version, app->date, app->time);
    ESP_LOGI(TAG, "IDF %s | device id %s | reset reason %d", IDF_VER, id, (int)esp_reset_reason());
    ESP_LOGI(TAG, "free heap %" PRIu32 " B", esp_get_free_heap_size());
}

/** A full NVS partition or a layout change from an older build must not block provisioning. */
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(app_config_init());
    banner();

    const app_config_t *cfg = app_config_get();

    const status_led_config_t led = {
        .enabled = cfg->led.enabled,
        .gpio = cfg->led.gpio,
        .brightness = cfg->led.brightness,
    };
    ESP_ERROR_CHECK(status_led_init(&led));

    const dali_bus_config_t bus = {
        .tx_gpio = cfg->dali.tx_gpio,
        .rx_gpio = cfg->dali.rx_gpio,
        .invert_tx = cfg->dali.invert_tx,
        .invert_rx = cfg->dali.invert_rx,
        .poll_interval_s = cfg->dali.poll_interval_s,
        .scan_on_boot = cfg->dali.scan_on_boot,
        .identify_blink_ms = cfg->dali.identify_blink_ms,
    };
    ESP_ERROR_CHECK(dali_bus_init(&bus));

    ESP_ERROR_CHECK(factory_button_init());

    /* Network last among the producers: the bus must be able to answer before anything can ask. */
    ESP_ERROR_CHECK(net_wifi_init());
    ESP_ERROR_CHECK(http_iface_init());
    ESP_ERROR_CHECK(mqtt_iface_init());

    /* Last: the guard needs every producer of its two conditions already running. */
    ESP_ERROR_CHECK(ota_validate_init());

    ESP_LOGI(TAG, "init complete (main task stack headroom %u B)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "factory_button.h"
#include "gw_events.h"

static const char *TAG = "gw";

#define POLL_MS 50

/*
 * Polled rather than interrupt-driven: the useful signal is "still held after N seconds", which an
 * edge interrupt cannot give without a timer anyway, and polling a single pin at 20 Hz is free.
 */
static void button_task(void *arg)
{
    (void)arg;
    uint16_t held_ms = 0;
    bool fired = false;

    for (;;) {
        bool down = gpio_get_level(CONFIG_GW_BUTTON_GPIO) == 0;

        if (!down) {
            if (held_ms > 0 && !fired) {
                gw_event_factory_reset_t ev = {.held_ms = 0, .triggered = false};
                gw_event_post(GW_EVENT_FACTORY_RESET_HOLD, &ev, sizeof(ev));
            }
            held_ms = 0;
            fired = false;
        } else if (!fired) {
            held_ms = (uint16_t)(held_ms + POLL_MS);
            gw_event_factory_reset_t ev = {.held_ms = held_ms, .triggered = false};
            gw_event_post(GW_EVENT_FACTORY_RESET_HOLD, &ev, sizeof(ev));

            if (held_ms >= CONFIG_GW_FACTORY_RESET_HOLD_MS) {
                fired = true;
                ESP_LOGW(TAG, "factory reset: button held %u ms", held_ms);
                gw_event_factory_reset_t done = {.held_ms = held_ms, .triggered = true};
                gw_event_post(GW_EVENT_FACTORY_RESET_HOLD, &done, sizeof(done));

                if (app_config_factory_reset() != ESP_OK) {
                    ESP_LOGE(TAG, "could not erase configuration");
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t factory_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_GW_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(button_task, "factory_btn", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "factory reset on GPIO%d, hold %d ms", CONFIG_GW_BUTTON_GPIO,
             CONFIG_GW_FACTORY_RESET_HOLD_MS);
    return ESP_OK;
}

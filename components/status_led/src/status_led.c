#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "gw_events.h"
#include "status_led.h"

static const char *TAG = "led";

#define TICK_MS 20
#define BLIP_MS 20
#define BREATH_PERIOD 3000
#define BLINK_PERIOD 600
#define FAST_PERIOD 160

/** Fraction of the configured brightness used by the "very dim steady" connected pattern. */
#define DIM_NUM 1
#define DIM_DEN 12

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static led_strip_handle_t s_strip;
static uint8_t s_brightness = 32;

/* Written from the default event loop, read from the LED task. Atomics avoid a lock on a path that
 * status_led_blip() may hit from the bus task between two DALI transactions. */
static _Atomic int s_net_state = GW_NET_BOOT;
static _Atomic bool s_mqtt_up = false;
static _Atomic bool s_bus_error = false;
static _Atomic bool s_ota = false;
static _Atomic bool s_factory_hold = false;
static _Atomic int s_override = STATUS_LED_OFF;
static _Atomic int64_t s_blip_until_us = 0;

static status_led_pattern_t effective_pattern(void)
{
    status_led_pattern_t ov = (status_led_pattern_t)atomic_load(&s_override);
    if (ov != STATUS_LED_OFF) {
        return ov;
    }
    if (atomic_load(&s_factory_hold)) {
        return STATUS_LED_FACTORY_RESET;
    }
    if (atomic_load(&s_ota)) {
        return STATUS_LED_OTA;
    }
    if (atomic_load(&s_bus_error)) {
        return STATUS_LED_BUS_ERROR;
    }
    switch ((gw_net_state_t)atomic_load(&s_net_state)) {
        case GW_NET_AP_PROVISIONING:
        case GW_NET_STA_FALLBACK_AP:
            return STATUS_LED_AP_PROVISIONING;
        case GW_NET_STA_CONNECTED:
            return atomic_load(&s_mqtt_up) ? STATUS_LED_CONNECTED_MQTT
                                           : STATUS_LED_CONNECTED_NOMQTT;
        case GW_NET_BOOT:
        case GW_NET_STA_CONNECTING:
        default:
            return STATUS_LED_STA_CONNECTING;
    }
}

/** Triangle ramp 0..255..0 over @p period_ms, squared so the eye reads it as a smooth breath. */
static uint8_t breath(uint32_t now_ms, uint32_t period_ms)
{
    uint32_t phase = now_ms % period_ms;
    uint32_t half = period_ms / 2;
    uint32_t up = phase < half ? phase : period_ms - phase;
    uint32_t lin = (up * 255u) / half;
    return (uint8_t)((lin * lin) / 255u);
}

static bool blink_on(uint32_t now_ms, uint32_t period_ms)
{
    return (now_ms % period_ms) < (period_ms / 2);
}

/** Two short pulses at the start of every @p period_ms window. */
static bool double_blink_on(uint32_t now_ms, uint32_t period_ms)
{
    uint32_t phase = now_ms % period_ms;
    return phase < 80u || (phase >= 200u && phase < 280u);
}

static rgb_t pattern_color(status_led_pattern_t pattern, uint32_t now_ms)
{
    switch (pattern) {
        case STATUS_LED_AP_PROVISIONING: {
            uint8_t v = breath(now_ms, BREATH_PERIOD);
            return (rgb_t){0, 0, v};
        }
        case STATUS_LED_STA_CONNECTING:
            return blink_on(now_ms, BLINK_PERIOD) ? (rgb_t){255, 160, 0} : (rgb_t){0, 0, 0};
        case STATUS_LED_CONNECTED_NOMQTT:
            return double_blink_on(now_ms, 3000) ? (rgb_t){0, 255, 0} : (rgb_t){0, 0, 0};
        case STATUS_LED_CONNECTED_MQTT:
            return (rgb_t){0, (uint8_t)(255 * DIM_NUM / DIM_DEN), 0};
        case STATUS_LED_BUS_ERROR:
            return blink_on(now_ms, BLINK_PERIOD) ? (rgb_t){255, 0, 0} : (rgb_t){0, 0, 0};
        case STATUS_LED_OTA: {
            uint8_t v = breath(now_ms, 1500);
            return (rgb_t){(uint8_t)(v * 160 / 255), 0, v};
        }
        case STATUS_LED_FACTORY_RESET:
            return blink_on(now_ms, FAST_PERIOD) ? (rgb_t){255, 0, 0} : (rgb_t){0, 0, 0};
        case STATUS_LED_OFF:
        default:
            return (rgb_t){0, 0, 0};
    }
}

static void led_task(void *arg)
{
    (void)arg;
    rgb_t last = {1, 1, 1}; /* forces the first refresh */

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        rgb_t c;

        if (esp_timer_get_time() < atomic_load(&s_blip_until_us)) {
            c = (rgb_t){255, 255, 255};
        } else {
            c = pattern_color(effective_pattern(), now_ms);
        }

        uint8_t scale = s_brightness;
        c.r = (uint8_t)((c.r * scale) / 255u);
        c.g = (uint8_t)((c.g * scale) / 255u);
        c.b = (uint8_t)((c.b * scale) / 255u);

        if (memcmp(&c, &last, sizeof(c)) != 0) {
            led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
            led_strip_refresh(s_strip);
            last = c;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

static void on_gw_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch ((gw_event_id_t)id) {
        case GW_EVENT_NET_STATE:
            atomic_store(&s_net_state, ((const gw_event_net_t *)data)->state);
            break;
        case GW_EVENT_MQTT_STATE:
            atomic_store(&s_mqtt_up, ((const gw_event_mqtt_t *)data)->connected);
            break;
        case GW_EVENT_BUS_STATE:
            atomic_store(&s_bus_error, !((const gw_event_bus_t *)data)->powered);
            break;
        case GW_EVENT_BUS_ACTIVITY:
            status_led_blip();
            break;
        case GW_EVENT_OTA:
            atomic_store(&s_ota, ((const gw_event_ota_t *)data)->in_progress);
            break;
        case GW_EVENT_FACTORY_RESET_HOLD: {
            const gw_event_factory_reset_t *ev = data;
            atomic_store(&s_factory_hold, !ev->triggered && ev->held_ms > 0);
            break;
        }
        default:
            break;
    }
}

esp_err_t status_led_init(const status_led_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg->enabled || cfg->gpio < 0) {
        ESP_LOGI(TAG, "status LED disabled");
        return ESP_OK;
    }

    s_brightness = cfg->brightness;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = cfg->gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0, /* auto: the C6 has 48-word channels, not 64 */
        .flags = {.with_dma = false},
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init on GPIO%d failed: %s", cfg->gpio, esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);

    err = esp_event_handler_register(DALI_GW_EVENT, ESP_EVENT_ANY_ID, on_gw_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(led_task, "status_led", 2560, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "status LED on GPIO%d, brightness %u", cfg->gpio, cfg->brightness);
    return ESP_OK;
}

void status_led_set(status_led_pattern_t pattern)
{
    atomic_store(&s_override, (int)pattern);
}

void status_led_blip(void)
{
    if (s_strip != NULL) {
        atomic_store(&s_blip_until_us, esp_timer_get_time() + BLIP_MS * 1000);
    }
}

void status_led_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

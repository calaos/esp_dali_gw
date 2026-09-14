/**
 * @file status_led.h
 * @brief RGB status LED patterns driven by system events (SPEC 11).
 *
 * The component subscribes to DALI_GW_EVENT itself and needs no wiring beyond init; callers only
 * override the pattern for states that are not expressible as an event (OTA, button hold).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ordered by ascending priority: a higher value wins when several conditions hold at once. */
typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_CONNECTED_MQTT,   /**< green, very dim steady */
    STATUS_LED_CONNECTED_NOMQTT, /**< green, short double blink every 3 s */
    STATUS_LED_STA_CONNECTING,   /**< yellow blink */
    STATUS_LED_AP_PROVISIONING,  /**< blue, slow breathing */
    STATUS_LED_BUS_ERROR,        /**< red blink */
    STATUS_LED_OTA,              /**< purple breathing */
    STATUS_LED_FACTORY_RESET,    /**< red fast blink */
} status_led_pattern_t;

typedef struct {
    bool enabled;
    int8_t gpio;
    uint8_t brightness; /**< 0..255, scales every pattern */
} status_led_config_t;

/** @brief Start the LED task and subscribe to DALI_GW_EVENT. */
esp_err_t status_led_init(const status_led_config_t *cfg);

/** @brief Force a pattern regardless of events. Pass STATUS_LED_OFF to return to event control. */
void status_led_set(status_led_pattern_t pattern);

/** @brief One brief white flicker (<= 20 ms) overlaid on the current pattern. */
void status_led_blip(void);

/** @brief Apply a brightness change without restarting. */
void status_led_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif

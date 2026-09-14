# status_led

Drives the on-board WS2812 RGB LED so the device is diagnosable without a serial console or a
browser. The component subscribes to `DALI_GW_EVENT` itself and derives the pattern from network,
MQTT, bus, OTA and factory-reset state; nothing else has to push state into it. Conditions are
ranked by the order of `status_led_pattern_t`, so the most urgent one wins when several hold at
once. A 20 ms tick task recomputes the colour and only touches the RMT channel when the pixel
actually changes.

## Public API

- `status_led_init(cfg)` — create the strip, subscribe to events, start the tick task. A disabled
  LED or a negative GPIO is a successful no-op.
- `status_led_set(pattern)` — force a pattern; `STATUS_LED_OFF` returns to event-driven control.
- `status_led_blip()` — one brief white flicker overlaid on the current pattern, for bus activity.
- `status_led_set_brightness(b)` — apply a brightness change without restarting.

Functional since M0. Patterns are specified in SPEC §11.

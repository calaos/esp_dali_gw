# gw_events

The one `esp_event` base (`DALI_GW_EVENT`) used for every cross-component notification (SPEC 4).
Components never expose FreeRTOS handles to each other, so this is the only coupling between the bus
task, the adapters and the LED. Payloads are small PODs copied into the loop; anything larger — the
gear list above all — is pulled from the `dali_bus` registry by the consumer instead of being posted.

## API

- `DALI_GW_EVENT` — the event base; ids are `gw_event_id_t`.
- `gw_event_post()` — post a POD payload on the default loop with zero block time.

Live now. The events themselves start firing as their producers land (net in **M1**, bus and MQTT in
**M2**).

# net_wifi

Network bring-up: the boot state machine, STA with backoff, SoftAP plus captive portal, and mDNS
(SPEC 5). It owns the decision of when to fall back to AP provisioning so no other component has to
reason about connection state; everyone else follows `GW_EVENT_NET_STATE`.

## API

- `net_wifi_init()` — bring up netif and Wi-Fi, start the state machine, return immediately.
- `net_wifi_get_status()` — IP, hostname, RSSI and mode for `/api/info`.
- `net_wifi_state()` — current boot state, cheaper than a full status read.
- `net_wifi_scan()` — blocking, bounded AP scan for the setup wizard.
- `net_wifi_ap_active()` — true while the SoftAP is up, i.e. the captive-portal redirect applies.

Functional in **M1**.

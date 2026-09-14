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

## Boot state machine

```
BOOT ─► wifi.ssid set? ─no─► AP_PROVISIONING
           │yes
           ▼
      STA_CONNECTING ──got ip──► STA_CONNECTED
           │                          ▲
           │ wifi.fallback_ap_timeout_s (default 60 s)
           ▼                          │
      STA_FALLBACK_AP ────────────────┘  (AP dropped 30 s after the association)
```

Every transition posts `GW_EVENT_NET_STATE`. Reconnection uses an exponential backoff of 1 s
doubling to a 30 s cap and never gives up; the backoff resets on association. Losing an established
link re-arms the fallback timer, so a router that disappears for good ends with the recovery AP up.

One `net` task owns the machine. Wi-Fi and IP handlers run on the default event loop and do nothing
but log and push an event onto its queue; every delay — connect backoff, fallback timeout, AP grace
period — is a deadline the task waits on with a bounded queue receive, so there is no timer callback
doing radio work and no busy-wait anywhere.

## SoftAP and captive portal

Both interfaces are configured once, while the radio is still stopped, so raising the recovery AP is
only a mode change. The AP is `ESP-DALI-GW-<id>` on 192.168.4.1/24, WPA2 with `wifi.ap_password`
(open, with a warning, if that password is shorter than the 8 characters WPA2 requires), 4 clients,
channel 6. The radio runs in APSTA whenever the AP is up: the setup wizard has to be able to scan.

`src/dns_hijack.c` is the DNS half of the captive portal — a UDP socket on port 53 that answers
every A query with the SoftAP address and replies NOERROR/no-answer to anything else, so an OS probe
never hangs. It runs only while the AP is up. The HTTP half (probe URLs and foreign `Host` headers)
belongs to `http_iface`, which gates it on `net_wifi_ap_active()`.

## mDNS

Started as soon as the radio is up — on the AP too, so `<hostname>.local` resolves during
provisioning — advertising `_http._tcp` on port 80 with `device.name` as the instance name.

## Known gaps

- The country code is fixed to `FR`: `app_config` has no country field. The AP channel is kept
  inside 1-11 so it stays legal regardless.
- Fallback to the recovery AP is driven by the timeout only, not by a failure count.

Functional in **M1**.

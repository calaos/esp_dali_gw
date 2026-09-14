/**
 * @file net_wifi.h
 * @brief Boot state machine, STA with backoff, SoftAP + captive portal, mDNS (SPEC 5).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "gw_events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_WIFI_MAX_SCAN_RESULTS 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    /** "open", "wep", "wpa", "wpa2", "wpa3", "wpa2_wpa3", "wpa_wpa2" */
    char auth[10];
} net_wifi_ap_t;

typedef struct {
    gw_net_state_t state;
    char ip[16];
    char hostname[33];
    int8_t rssi;
    /** "sta", "ap" or "apsta" — reported verbatim on /api/info. */
    char mode[6];
} net_wifi_status_t;

/**
 * @brief Bring up netif, Wi-Fi and the boot state machine.
 *
 * Returns as soon as the state machine is started; connection progress arrives as
 * GW_EVENT_NET_STATE. An empty wifi.ssid goes straight to AP provisioning.
 */
esp_err_t net_wifi_init(void);

void net_wifi_get_status(net_wifi_status_t *out);

/** @brief Current boot state; cheaper than net_wifi_get_status() for a single check. */
gw_net_state_t net_wifi_state(void);

/**
 * @brief Blocking scan for the setup wizard.
 *
 * @param out       array of at least @p max entries
 * @param max       capacity of @p out
 * @param[out] found number of entries written
 * @param timeout_ms bounded by the caller; the HTTP layer allows 5 s (SPEC 9)
 */
esp_err_t net_wifi_scan(net_wifi_ap_t *out, size_t max, size_t *found, uint32_t timeout_ms);

/** @brief True while the SoftAP is up, i.e. the captive-portal redirect must be served. */
bool net_wifi_ap_active(void);

#ifdef __cplusplus
}
#endif

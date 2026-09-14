#include <string.h>

#include "esp_log.h"

#include "net_wifi.h"

static const char *TAG = "net";

esp_err_t net_wifi_init(void)
{
    // TODO(M1): netif + Wi-Fi init, boot state machine, STA backoff, SoftAP fallback, mDNS.
    ESP_LOGW(TAG, "stub: no radio is started, state stays boot");
    return ESP_OK;
}

void net_wifi_get_status(net_wifi_status_t *out)
{
    // TODO(M1)
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = GW_NET_BOOT;
}

gw_net_state_t net_wifi_state(void)
{
    // TODO(M1)
    return GW_NET_BOOT;
}

esp_err_t net_wifi_scan(net_wifi_ap_t *out, size_t max, size_t *found, uint32_t timeout_ms)
{
    // TODO(M1)
    (void)out;
    (void)max;
    (void)timeout_ms;
    if (found != NULL) {
        *found = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool net_wifi_ap_active(void)
{
    // TODO(M1)
    return false;
}

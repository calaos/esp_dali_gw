#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "app_config.h"
#include "time_sync.h"

static const char *TAG = "net";

static bool s_started;

/** The epoch the RTC starts from; anything beyond it means a server actually answered. */
#define PLAUSIBLE_EPOCH 1700000000

void time_sync_start(void)
{
    if (s_started) {
        return;
    }

    const char *tz = app_config_get()->device.timezone;
    if (tz[0] != '\0') {
        /* POSIX TZ, not an IANA name: esp_libc has no tzdata, so this only shifts UTC. */
        setenv("TZ", tz, 1);
        tzset();
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s; timestamps stay relative", esp_err_to_name(err));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

bool time_sync_ready(void)
{
    return time(NULL) > PLAUSIBLE_EPOCH;
}

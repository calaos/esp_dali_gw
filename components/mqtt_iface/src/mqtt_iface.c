#include "esp_log.h"

#include "mqtt_iface.h"

static const char *TAG = "mqtt";

esp_err_t mqtt_iface_init(void)
{
    // TODO(M2): esp-mqtt client, LWT, topic routing, retained state.
    ESP_LOGW(TAG, "stub: no broker connection");
    return ESP_OK;
}

esp_err_t mqtt_iface_restart(void)
{
    // TODO(M2)
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mqtt_iface_stop(void)
{
    // TODO(M2)
    return ESP_OK;
}

bool mqtt_iface_connected(void)
{
    // TODO(M2)
    return false;
}

esp_err_t mqtt_iface_publish_all(void)
{
    // TODO(M2)
    return ESP_ERR_NOT_SUPPORTED;
}

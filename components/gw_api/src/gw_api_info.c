#include <string.h>
#include "gw_api.h"

/** Fields shared by the MQTT status topic and /api/info, so the two can never drift. */
static void add_status_fields(cJSON *obj, const gw_info_t *info)
{
    cJSON_AddStringToObject(obj, "state", info->state);
    cJSON_AddStringToObject(obj, "fw", info->fw);
    cJSON_AddStringToObject(obj, "idf", info->idf);
    cJSON_AddStringToObject(obj, "ip", info->ip);
    cJSON_AddNumberToObject(obj, "rssi", info->rssi);
    cJSON_AddNumberToObject(obj, "uptime_s", info->uptime_s);
    cJSON_AddStringToObject(obj, "mac", info->mac);
}

cJSON *gw_api_status_to_json(const gw_info_t *info)
{
    if (info == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    add_status_fields(obj, info);
    return obj;
}

cJSON *gw_api_info_to_json(const gw_info_t *info)
{
    if (info == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    add_status_fields(obj, info);
    cJSON_AddStringToObject(obj, "hostname", info->hostname);
    cJSON_AddStringToObject(obj, "device_id", info->device_id);
    cJSON_AddStringToObject(obj, "mode", info->mode);
    cJSON_AddStringToObject(obj, "chip", info->chip);
    cJSON_AddStringToObject(obj, "build_date", info->build_date);
    cJSON_AddNumberToObject(obj, "free_heap", info->free_heap);
    cJSON_AddNumberToObject(obj, "min_free_heap", info->min_free_heap);
    cJSON_AddNumberToObject(obj, "reset_reason", info->reset_reason);
    return obj;
}

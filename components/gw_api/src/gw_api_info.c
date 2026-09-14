#include <string.h>

#include "gw_api.h"

cJSON *gw_api_status_to_json(const gw_info_t *info)
{
    if (info == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "state", info->state);
    cJSON_AddStringToObject(obj, "fw", info->fw);
    cJSON_AddStringToObject(obj, "idf", info->idf);
    cJSON_AddStringToObject(obj, "ip", info->ip);
    /* No STA association means no meaningful RSSI; the field is absent rather than a fake 0. */
    if (strcmp(info->mode, "ap") != 0) {
        cJSON_AddNumberToObject(obj, "rssi", info->rssi);
    }
    cJSON_AddNumberToObject(obj, "uptime_s", info->uptime_s);
    cJSON_AddStringToObject(obj, "mac", info->mac);
    return obj;
}

cJSON *gw_api_info_to_json(const gw_info_t *info)
{
    if (info == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON *status = gw_api_status_to_json(info);
    cJSON *build = cJSON_CreateObject();
    if (obj == NULL || status == NULL || build == NULL) {
        cJSON_Delete(obj);
        cJSON_Delete(status);
        cJSON_Delete(build);
        return NULL;
    }

    /* The status object is nested and byte-identical to the MQTT status topic (SPEC 8.1, 9). */
    cJSON_AddItemToObject(obj, "status", status);

    cJSON_AddStringToObject(build, "version", info->fw);
    cJSON_AddStringToObject(build, "date", info->build_date);
    cJSON_AddStringToObject(build, "idf", info->idf);
    cJSON_AddStringToObject(build, "target", info->chip);
    cJSON_AddItemToObject(obj, "build", build);

    cJSON_AddStringToObject(obj, "mode", info->mode);
    cJSON_AddStringToObject(obj, "hostname", info->hostname);
    cJSON_AddStringToObject(obj, "device_id", info->device_id);
    cJSON_AddNumberToObject(obj, "free_heap", info->free_heap);
    cJSON_AddNumberToObject(obj, "min_free_heap", info->min_free_heap);
    cJSON_AddNumberToObject(obj, "reset_reason", info->reset_reason);
    return obj;
}

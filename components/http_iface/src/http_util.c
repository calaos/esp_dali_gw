#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_util.h"

static const char *TAG = "http";

esp_err_t http_read_body(httpd_req_t *req, char **out, size_t *len)
{
    *out = NULL;
    if (len != NULL) {
        *len = 0;
    }
    size_t total = req->content_len;
    if (total > HTTP_MAX_BODY) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(total + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    buf[total] = '\0';
    *out = buf;
    if (len != NULL) {
        *len = total;
    }
    return ESP_OK;
}

esp_err_t http_read_json(httpd_req_t *req, cJSON **out)
{
    *out = NULL;
    char *body = NULL;
    esp_err_t err = http_read_body(req, &body, NULL);
    if (err != ESP_OK) {
        http_send_error(req, err == ESP_ERR_INVALID_SIZE ? GW_ERR_INVALID_ARG : GW_ERR_INTERNAL,
                        err == ESP_ERR_INVALID_SIZE ? "body too large" : "cannot read body");
        return err;
    }

    /* An empty body is a valid "no arguments" payload for the confirm-style routes. */
    *out = (body[0] == '\0') ? cJSON_CreateObject() : cJSON_Parse(body);
    free(body);
    if (*out == NULL) {
        http_send_error(req, GW_ERR_INVALID_ARG, "malformed JSON");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t http_send_json(httpd_req_t *req, cJSON *obj)
{
    if (obj == NULL) {
        return http_send_error(req, GW_ERR_INTERNAL, "serialization failed");
    }
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        return http_send_error(req, GW_ERR_INTERNAL, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

/** The closed error set maps onto HTTP status so a client can react without parsing the body. */
static const char *status_line(gw_err_t err)
{
    switch (err) {
        case GW_ERR_INVALID_ARG:
            return "400 Bad Request";
        case GW_ERR_NOT_PRESENT:
            return "404 Not Found";
        case GW_ERR_ADDRESS_IN_USE:
            return "409 Conflict";
        case GW_ERR_UNSUPPORTED:
            return "501 Not Implemented";
        case GW_ERR_BUS_BUSY:
            return "503 Service Unavailable";
        case GW_ERR_TIMEOUT:
        case GW_ERR_NO_REPLY:
            return "504 Gateway Timeout";
        default:
            return "500 Internal Server Error";
    }
}

esp_err_t http_send_error(httpd_req_t *req, gw_err_t err, const char *message)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", gw_api_err_str(err));
    if (message != NULL) {
        cJSON_AddStringToObject(obj, "message", message);
    }
    ESP_LOGW(TAG, "%s %s: %s (%s)", http_method_str(req->method), req->uri, gw_api_err_str(err),
             message ? message : "");
    httpd_resp_set_status(req, status_line(err));
    return http_send_json(req, obj);
}

bool http_query_flag(httpd_req_t *req, const char *key)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 128) {
        return false;
    }
    char query[129];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[8];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}

bool http_confirmed(const cJSON *root)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(root, "confirm"));
}

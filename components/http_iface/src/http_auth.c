#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/base64.h"

#include "app_config.h"
#include "http_auth.h"

static const char *TAG = "http";

#define AUTH_PREFIX "Basic "
#define AUTH_PREFIX_LEN 6

/** Constant-time compare so a wrong password cannot be found one byte at a time. */
static bool secure_equal(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0 && la == lb;
}

static bool challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"esp_dali_gw\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_arg\","
                            "\"message\":\"authentication required\"}");
    return false;
}

bool http_auth_ok(httpd_req_t *req)
{
    const app_config_http_auth_t *auth = &app_config_get()->http.auth;
    if (!auth->enabled) {
        return true;
    }

    char header[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return challenge(req);
    }
    if (strncmp(header, AUTH_PREFIX, AUTH_PREFIX_LEN) != 0) {
        return challenge(req);
    }

    unsigned char decoded[96];
    size_t decoded_len = 0;
    const unsigned char *b64 = (const unsigned char *)header + AUTH_PREFIX_LEN;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, b64,
                              strlen((char *)b64)) != 0) {
        return challenge(req);
    }
    decoded[decoded_len] = '\0';

    char *sep = strchr((char *)decoded, ':');
    if (sep == NULL) {
        return challenge(req);
    }
    *sep = '\0';

    bool ok =
        secure_equal((char *)decoded, auth->username) && secure_equal(sep + 1, auth->password);
    /* Wipe the decoded credentials rather than leaving them on the httpd task stack. */
    memset(decoded, 0, sizeof(decoded));

    if (!ok) {
        ESP_LOGW(TAG, "rejected credentials for %s", req->uri);
        return challenge(req);
    }
    return true;
}

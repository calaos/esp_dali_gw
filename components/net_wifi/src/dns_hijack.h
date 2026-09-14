/**
 * @file dns_hijack.h
 * @brief Captive-portal DNS responder: every A query is answered with the SoftAP address.
 *
 * Private to net_wifi. Only the HTTP redirect side of the portal lives in http_iface.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind UDP/53 and start answering.
 *
 * @param addr_be the address handed out, in network byte order (the SoftAP netif IP)
 */
esp_err_t dns_hijack_start(uint32_t addr_be);

/** @brief Stop and join the responder task. Safe to call when it is not running. */
void dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif

/**
 * @file webui.h
 * @brief Lookup table over the gzipped web UI assets embedded at build time.
 *
 * web/dist is gzipped by CMake and embedded with EMBED_FILES; this exposes the result so
 * http_iface can serve it without knowing how the embedding works.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *path;         /**< request path, leading slash included ("/index.html") */
    const char *content_type; /**< full header value, charset included */
    const uint8_t *data;      /**< gzip-compressed bytes */
    size_t size;
} webui_asset_t;

/**
 * @brief Find an asset by request path.
 *
 * "/" resolves to the SPA entry point. Unknown paths return NULL; the caller decides between the
 * captive-portal redirect and a 404.
 */
const webui_asset_t *webui_find(const char *path);

/** @brief SPA entry point, for the captive portal and client-side routing fallbacks. */
const webui_asset_t *webui_index(void);

/** @brief Build hash of the embedded bundle, used verbatim as the ETag. */
const char *webui_build_hash(void);

#ifdef __cplusplus
}
#endif

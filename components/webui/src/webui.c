#include <string.h>
#include "webui.h"

/* Defined in the generated webui_assets.c. */
extern const webui_asset_t webui_assets[];
extern const size_t webui_asset_count;
extern const char webui_asset_hash[];

const webui_asset_t *webui_index(void)
{
    return webui_find("/index.html");
}

const webui_asset_t *webui_find(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    if (path[0] == '/' && path[1] == '\0') {
        return webui_find("/index.html");
    }
    for (size_t i = 0; i < webui_asset_count; i++) {
        if (strcmp(webui_assets[i].path, path) == 0) {
            return &webui_assets[i];
        }
    }
    return NULL;
}

const char *webui_build_hash(void)
{
    return webui_asset_hash;
}

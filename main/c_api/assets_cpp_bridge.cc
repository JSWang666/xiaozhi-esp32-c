#include "assets_c_api.h"
#include "assets.h"

struct assets_handle {
    Assets *impl;
};

static assets_handle_t s_assets_handle;

assets_handle_t *assets_get_instance(void) {
    s_assets_handle.impl = &Assets::GetInstance();
    return &s_assets_handle;
}

bool assets_partition_valid(const assets_handle_t *assets) {
    return assets && assets->impl && assets->impl->partition_valid();
}

const char *assets_default_url(const assets_handle_t *assets) {
    if (assets == nullptr || assets->impl == nullptr) return "";
    static std::string url;
    url = assets->impl->default_assets_url();
    return url.c_str();
}

bool assets_download(assets_handle_t *assets, const char *url,
                     assets_progress_cb_t cb, void *ctx) {
    if (assets == nullptr || assets->impl == nullptr || url == nullptr) return false;
    return assets->impl->Download(std::string(url),
        [cb, ctx](int progress, size_t speed) {
            if (cb) cb(progress, speed, ctx);
        });
}

bool assets_apply(assets_handle_t *assets, bool refresh_display_theme) {
    if (assets == nullptr || assets->impl == nullptr) return false;
    return assets->impl->Apply(refresh_display_theme);
}

bool assets_get_data(assets_handle_t *assets, const char *name,
                     void **ptr, size_t *size) {
    if (assets == nullptr || assets->impl == nullptr ||
        name == nullptr || ptr == nullptr || size == nullptr) return false;
    return assets->impl->GetAssetData(std::string(name), *ptr, *size);
}

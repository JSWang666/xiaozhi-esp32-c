#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>

#define TAG "Settings"

struct settings_t {
    nvs_handle_t nvs_handle;
    bool read_write;
    bool dirty;
};

settings_t *settings_open(const char *ns, bool read_write)
{
    settings_t *s = (settings_t *)calloc(1, sizeof(settings_t));
    if (!s) return NULL;

    s->read_write = read_write;
    esp_err_t err = nvs_open(ns, read_write ? NVS_READWRITE : NVS_READONLY, &s->nvs_handle);
    if (err != ESP_OK) {
        s->nvs_handle = 0;
    }
    return s;
}

void settings_close(settings_t *handle)
{
    if (!handle) return;
    if (handle->nvs_handle != 0) {
        if (handle->read_write && handle->dirty) {
            ESP_ERROR_CHECK(nvs_commit(handle->nvs_handle));
        }
        nvs_close(handle->nvs_handle);
    }
    free(handle);
}

char *settings_get_string(settings_t *handle, const char *key, const char *default_value)
{
    if (!handle || handle->nvs_handle == 0) {
        return default_value ? strdup(default_value) : NULL;
    }

    size_t length = 0;
    if (nvs_get_str(handle->nvs_handle, key, NULL, &length) != ESP_OK) {
        return default_value ? strdup(default_value) : NULL;
    }

    char *buf = (char *)malloc(length);
    if (!buf) return NULL;

    ESP_ERROR_CHECK(nvs_get_str(handle->nvs_handle, key, buf, &length));

    /* Trim trailing null bytes (matches original C++ behavior) */
    while (length > 0 && buf[length - 1] == '\0') {
        length--;
    }
    buf[length] = '\0';
    return buf;
}

void settings_set_string(settings_t *handle, const char *key, const char *value)
{
    if (!handle) return;
    if (handle->read_write) {
        ESP_ERROR_CHECK(nvs_set_str(handle->nvs_handle, key, value));
        handle->dirty = true;
    } else {
        ESP_LOGW(TAG, "Namespace is not open for writing");
    }
}

int32_t settings_get_int(settings_t *handle, const char *key, int32_t default_value)
{
    if (!handle || handle->nvs_handle == 0) {
        return default_value;
    }
    int32_t value;
    if (nvs_get_i32(handle->nvs_handle, key, &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void settings_set_int(settings_t *handle, const char *key, int32_t value)
{
    if (!handle) return;
    if (handle->read_write) {
        ESP_ERROR_CHECK(nvs_set_i32(handle->nvs_handle, key, value));
        handle->dirty = true;
    } else {
        ESP_LOGW(TAG, "Namespace is not open for writing");
    }
}

bool settings_get_bool(settings_t *handle, const char *key, bool default_value)
{
    if (!handle || handle->nvs_handle == 0) {
        return default_value;
    }
    uint8_t value;
    if (nvs_get_u8(handle->nvs_handle, key, &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void settings_set_bool(settings_t *handle, const char *key, bool value)
{
    if (!handle) return;
    if (handle->read_write) {
        ESP_ERROR_CHECK(nvs_set_u8(handle->nvs_handle, key, value ? 1 : 0));
        handle->dirty = true;
    } else {
        ESP_LOGW(TAG, "Namespace is not open for writing");
    }
}

void settings_erase_key(settings_t *handle, const char *key)
{
    if (!handle) return;
    if (handle->read_write) {
        esp_err_t ret = nvs_erase_key(handle->nvs_handle, key);
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_ERROR_CHECK(ret);
        }
    } else {
        ESP_LOGW(TAG, "Namespace is not open for writing");
    }
}

void settings_erase_all(settings_t *handle)
{
    if (!handle) return;
    if (handle->read_write) {
        ESP_ERROR_CHECK(nvs_erase_all(handle->nvs_handle));
    } else {
        ESP_LOGW(TAG, "Namespace is not open for writing");
    }
}

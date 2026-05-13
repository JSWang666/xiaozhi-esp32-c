#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <nvs_flash.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct settings_t settings_t;

settings_t *settings_open(const char *ns, bool read_write);
void settings_close(settings_t *handle);

/**
 * Get string value from NVS. Returns a malloc'd copy that caller must free().
 * Returns a malloc'd copy of default_value if key not found.
 * Returns NULL only on allocation failure.
 */
char *settings_get_string(settings_t *handle, const char *key, const char *default_value);
void settings_set_string(settings_t *handle, const char *key, const char *value);

int32_t settings_get_int(settings_t *handle, const char *key, int32_t default_value);
void settings_set_int(settings_t *handle, const char *key, int32_t value);

bool settings_get_bool(settings_t *handle, const char *key, bool default_value);
void settings_set_bool(settings_t *handle, const char *key, bool value);

void settings_erase_key(settings_t *handle, const char *key);
void settings_erase_all(settings_t *handle);

#ifdef __cplusplus
}
#endif

/* C++ compatibility wrapper for existing callers */
#ifdef __cplusplus
#include <string>
#include <cstdlib>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false)
        : handle_(settings_open(ns.c_str(), read_write)) {}
    ~Settings() { if (handle_) settings_close(handle_); }

    std::string GetString(const std::string& key, const std::string& default_value = "") {
        char *val = settings_get_string(handle_, key.c_str(), default_value.c_str());
        if (!val) return default_value;
        std::string result(val);
        free(val);
        return result;
    }
    void SetString(const std::string& key, const std::string& value) {
        settings_set_string(handle_, key.c_str(), value.c_str());
    }
    int32_t GetInt(const std::string& key, int32_t default_value = 0) {
        return settings_get_int(handle_, key.c_str(), default_value);
    }
    void SetInt(const std::string& key, int32_t value) {
        settings_set_int(handle_, key.c_str(), value);
    }
    bool GetBool(const std::string& key, bool default_value = false) {
        return settings_get_bool(handle_, key.c_str(), default_value);
    }
    void SetBool(const std::string& key, bool value) {
        settings_set_bool(handle_, key.c_str(), value);
    }
    void EraseKey(const std::string& key) {
        settings_erase_key(handle_, key.c_str());
    }
    void EraseAll() {
        settings_erase_all(handle_);
    }

private:
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    settings_t *handle_;
};
#endif

#endif /* SETTINGS_H */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "c_api/app_c_api.h"

#define TAG "main"

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    app_config_t cfg = {0};
    app_context_t *app = app_create(&cfg);
    ESP_ERROR_CHECK(app != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(app_init(app));
    ESP_ERROR_CHECK(app_run(app));
}

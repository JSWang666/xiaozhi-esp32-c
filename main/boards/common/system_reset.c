#include "system_reset.h"

#include <stdlib.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#define TAG "SystemReset"

struct system_reset {
    gpio_num_t reset_nvs_pin;
    gpio_num_t reset_factory_pin;
};

static void reset_nvs_flash(void)
{
    ESP_LOGI(TAG, "Resetting NVS flash");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS flash");
    }
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash");
    }
}

static void restart_in_seconds(int seconds)
{
    for (int i = seconds; i > 0; i--) {
        ESP_LOGI(TAG, "Resetting in %d seconds", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    esp_restart();
}

static void reset_to_factory(void)
{
    ESP_LOGI(TAG, "Resetting to factory");
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find otadata partition");
        return;
    }
    esp_partition_erase_range(partition, 0, partition->size);
    ESP_LOGI(TAG, "Erased otadata partition");

    restart_in_seconds(3);
}

system_reset_t *system_reset_create(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin)
{
    system_reset_t *sr = calloc(1, sizeof(*sr));
    if (!sr) return NULL;

    sr->reset_nvs_pin = reset_nvs_pin;
    sr->reset_factory_pin = reset_factory_pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << reset_nvs_pin) | (1ULL << reset_factory_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    return sr;
}

void system_reset_destroy(system_reset_t *sr)
{
    free(sr);
}

void system_reset_check_buttons(system_reset_t *sr)
{
    if (!sr) return;

    if (gpio_get_level(sr->reset_factory_pin) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset to factory");
        reset_nvs_flash();
        reset_to_factory();
    }

    if (gpio_get_level(sr->reset_nvs_pin) == 0) {
        ESP_LOGI(TAG, "Button is pressed, reset NVS flash");
        reset_nvs_flash();
    }
}

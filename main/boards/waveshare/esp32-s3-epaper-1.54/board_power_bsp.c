#include <stdlib.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

typedef struct {
    int epd_power_pin;
    int audio_power_pin;
    int vbat_power_pin;
} board_power_bsp_t;

board_power_bsp_t *board_power_bsp_create(int epd_power_pin, int audio_power_pin, int vbat_power_pin);
void board_power_epd_on(board_power_bsp_t *pwr);
void board_power_epd_off(board_power_bsp_t *pwr);
void board_power_audio_on(board_power_bsp_t *pwr);
void board_power_audio_off(board_power_bsp_t *pwr);
void board_power_vbat_on(board_power_bsp_t *pwr);
void board_power_vbat_off(board_power_bsp_t *pwr);

static void power_led_task(void *arg)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (0x1ULL << GPIO_NUM_3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    for (;;) {
        gpio_set_level(GPIO_NUM_3, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(GPIO_NUM_3, 1);
        vTaskDelay(pdMS_TO_TICKS(4800));
    }
}

board_power_bsp_t *board_power_bsp_create(int epd_power_pin, int audio_power_pin, int vbat_power_pin)
{
    board_power_bsp_t *pwr = calloc(1, sizeof(board_power_bsp_t));
    if (!pwr) return NULL;

    pwr->epd_power_pin = epd_power_pin;
    pwr->audio_power_pin = audio_power_pin;
    pwr->vbat_power_pin = vbat_power_pin;

    gpio_config_t gpio_conf = {
        .pin_bit_mask = (0x1ULL << epd_power_pin) | (0x1ULL << audio_power_pin) | (0x1ULL << vbat_power_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    xTaskCreatePinnedToCore(power_led_task, "PowerLedTask", 3 * 1024, NULL, 2, NULL, 0);

    return pwr;
}

void board_power_epd_on(board_power_bsp_t *pwr)
{
    gpio_set_level((gpio_num_t)pwr->epd_power_pin, 0);
}

void board_power_epd_off(board_power_bsp_t *pwr)
{
    gpio_set_level((gpio_num_t)pwr->epd_power_pin, 1);
}

void board_power_audio_on(board_power_bsp_t *pwr)
{
    gpio_set_level((gpio_num_t)pwr->audio_power_pin, 0);
}

void board_power_audio_off(board_power_bsp_t *pwr)
{
    gpio_set_level((gpio_num_t)pwr->audio_power_pin, 1);
}

void board_power_vbat_on(board_power_bsp_t *pwr)
{
    gpio_set_level((gpio_num_t)pwr->vbat_power_pin, 1);
}

void board_power_vbat_off(board_power_bsp_t *pwr)
{
    gpio_set_level((gpio_num_t)pwr->vbat_power_pin, 0);
}

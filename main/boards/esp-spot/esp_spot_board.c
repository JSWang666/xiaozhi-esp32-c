#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "led/circular_strip.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>

#define TAG "esp_spot"

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t i2c_bus;
    audio_codec_t *codec;
    led_t *led;
    board_btn_t *boot_button;
    board_btn_t *key_button;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t adc1_cali_handle;
    bool adc_calibration_ok;
} esp_spot_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_key_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_toggle_chat(app);
}

static void init_gpio(void)
{
    gpio_config_t io_pa = {
        .pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_pa);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 0);

    gpio_config_t io1 = {
        .pin_bit_mask = (1ULL << MCU_VCC_CTL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io1);

    gpio_config_t io2 = {
        .pin_bit_mask = (1ULL << PERP_VCC_CTL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io2);
}

static void init_power_ctl(void)
{
    init_gpio();
    gpio_set_level(MCU_VCC_CTL, 1);
    gpio_hold_en(MCU_VCC_CTL);
    gpio_set_level(PERP_VCC_CTL, 1);
    gpio_hold_en(PERP_VCC_CTL);
}

static void init_adc(esp_spot_ctx_t *ctx)
{
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &ctx->adc1_handle));

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(ctx->adc1_handle, VBAT_ADC_CHANNEL, &chan_config));

#ifdef ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_WIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &ctx->adc1_cali_handle) == ESP_OK) {
        ctx->adc_calibration_ok = true;
        ESP_LOGI(TAG, "ADC Curve Fitting calibration succeeded");
    }
#endif
}

static void init_i2c(esp_spot_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->i2c_bus));
}

static void init_buttons(esp_spot_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t key_cfg = { .gpio_num = KEY_BUTTON_GPIO };
    ctx->key_button = board_btn_create_gpio(&key_cfg);
    board_btn_on_click(ctx->key_button, on_key_click, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "esp-spot";
}

static void *get_led(board_desc_t *self)
{
    esp_spot_ctx_t *ctx = (esp_spot_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = circular_strip_led_create(LED_GPIO, 1);
    }
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    esp_spot_ctx_t *ctx = (esp_spot_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, false, false);
    }
    return ctx->codec;
}

static bool get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging)
{
    esp_spot_ctx_t *ctx = (esp_spot_ctx_t *)self;
    int raw_value = 0;
    int voltage = 0;

    ESP_ERROR_CHECK(adc_oneshot_read(ctx->adc1_handle, VBAT_ADC_CHANNEL, &raw_value));

    if (ctx->adc_calibration_ok) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(ctx->adc1_cali_handle, raw_value, &voltage));
        voltage = voltage * 3 / 2;
    } else {
        voltage = raw_value;
    }

    if (voltage < EMPTY_BATTERY_VOLTAGE) voltage = EMPTY_BATTERY_VOLTAGE;
    if (voltage > FULL_BATTERY_VOLTAGE) voltage = FULL_BATTERY_VOLTAGE;

    *level = (voltage - EMPTY_BATTERY_VOLTAGE) * 100 / (FULL_BATTERY_VOLTAGE - EMPTY_BATTERY_VOLTAGE);
    *charging = gpio_get_level(MCU_VCC_CTL);
    *discharging = !(*charging);
    return true;
}

static void destroy(board_desc_t *self)
{
    esp_spot_ctx_t *ctx = (esp_spot_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->key_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    esp_spot_ctx_t *ctx = calloc(1, sizeof(esp_spot_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = destroy;

    init_power_ctl();
    init_adc(ctx);
    init_i2c(ctx);
    init_buttons(ctx);

    return &ctx->base;
}

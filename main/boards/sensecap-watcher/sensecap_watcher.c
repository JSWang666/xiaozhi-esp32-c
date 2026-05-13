#include "board_defs.h"
#include "audio_codec.h"
#include "c_api/display_c_api.h"
#include "c_api/app_c_api.h"
#include "backlight.h"
#include "knob.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "config.h"
#include "device_state.h"
#include "assets/lang_c.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_check.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/spi_master.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <iot_button.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_sleep.h>
#include <esp_console.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <esp_app_desc.h>
#include <lvgl.h>

audio_codec_t *sensecap_audio_codec_create(void *i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7243e_addr, bool input_reference);

typedef struct sscma_camera sscma_camera_t;
sscma_camera_t *sscma_camera_create(esp_io_expander_handle_t io_exp_handle);
void sscma_camera_destroy(sscma_camera_t *self);

#define TAG "sensecap_watcher"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_exp_handle;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    led_t *led;
    board_knob_t *knob;
    power_save_timer_t *pst;
    sscma_camera_t *camera;

    button_handle_t btns;
    button_driver_t *btn_driver;
    uint32_t long_press_cnt;
} sensecap_ctx_t;

static sensecap_ctx_t *s_instance;

static esp_err_t io_expander_set_level(sensecap_ctx_t *ctx, uint16_t pin_mask, uint8_t level)
{
    return esp_io_expander_set_level(ctx->io_exp_handle, pin_mask, level);
}

static uint8_t io_expander_get_level(sensecap_ctx_t *ctx, uint16_t pin_mask)
{
    uint32_t pin_val = 0;
    esp_io_expander_get_level(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
    pin_mask &= DRV_IO_EXP_INPUT_MASK;
    return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
}

static void on_knob_rotate(bool clockwise, void *ud)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)ud;
    if (!ctx->codec) return;

    int current_volume = audio_codec_output_volume(ctx->codec);
    int new_volume = current_volume + (clockwise ? -5 : 5);
    if (new_volume > 100) new_volume = 100;
    else if (new_volume < 0) new_volume = 0;

    audio_codec_set_output_volume(ctx->codec, new_volume);
    ESP_LOGI(TAG, "Volume changed from %d to %d", current_volume, new_volume);

    if (ctx->display) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %d", lang_str_volume, audio_codec_output_volume(ctx->codec));
        display_show_notification(ctx->display, buf, 1000);
    }
    power_save_timer_wake_up(ctx->pst);
}

static void on_pst_enter_sleep(void *ud)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)ud;
    if (ctx->display) display_set_power_save_mode(ctx->display, true);
    if (ctx->backlight) backlight_set_brightness(ctx->backlight, 10, false);
}

static void on_pst_exit_sleep(void *ud)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)ud;
    if (ctx->display) display_set_power_save_mode(ctx->display, false);
    if (ctx->backlight) backlight_restore_brightness(ctx->backlight);
}

static void on_pst_shutdown(void *ud)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)ud;
    ESP_LOGI(TAG, "Shutting down");
    bool is_charging = (io_expander_get_level(ctx, BSP_PWR_VBUS_IN_DET) == 0);
    if (is_charging) {
        ESP_LOGI(TAG, "charging");
        if (ctx->backlight) backlight_set_brightness(ctx->backlight, 0, false);
    } else {
        io_expander_set_level(ctx, BSP_PWR_SYSTEM, 0);
    }
}

static void init_power_save(sensecap_ctx_t *ctx)
{
    power_save_timer_cfg_t cfg = { .cpu_max_freq = -1, .seconds_to_sleep = 60, .seconds_to_shutdown = 300 };
    ctx->pst = power_save_timer_create(&cfg);
    power_save_timer_on_enter_sleep(ctx->pst, on_pst_enter_sleep, ctx);
    power_save_timer_on_exit_sleep(ctx->pst, on_pst_exit_sleep, ctx);
    power_save_timer_on_shutdown(ctx->pst, on_pst_shutdown, ctx);
    power_save_timer_set_enabled(ctx->pst, true);
}

static void init_i2c(sensecap_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = BSP_GENERAL_I2C_SDA,
        .scl_io_num = BSP_GENERAL_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));

    const gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_I2C_SDA) | (1ULL << BSP_TOUCH_I2C_SCL) |
                        (1ULL << BSP_SPI3_HOST_PCLK) | (1ULL << BSP_SPI3_HOST_DATA0) |
                        (1ULL << BSP_SPI3_HOST_DATA1) | (1ULL << BSP_SPI3_HOST_DATA2) |
                        (1ULL << BSP_SPI3_HOST_DATA3) | (1ULL << BSP_LCD_SPI_CS) |
                        (1ULL << DISPLAY_BACKLIGHT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_config);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
    gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
    gpio_set_level(BSP_LCD_SPI_CS, 0);
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
    gpio_set_level(BSP_SPI3_HOST_PCLK, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA0, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA1, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA2, 0);
    gpio_set_level(BSP_SPI3_HOST_DATA3, 0);
}

static void init_expander(sensecap_ctx_t *ctx)
{
    esp_err_t ret = ESP_OK;
    esp_io_expander_new_i2c_tca95xx_16bit(ctx->i2c_bus, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_001, &ctx->io_exp_handle);

    ret |= esp_io_expander_set_dir(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);
    ret |= esp_io_expander_set_dir(ctx->io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
    ret |= esp_io_expander_set_level(ctx->io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, 0);
    ret |= esp_io_expander_set_level(ctx->io_exp_handle, BSP_PWR_SYSTEM, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ret |= esp_io_expander_set_level(ctx->io_exp_handle, BSP_PWR_START_UP, 1);
    vTaskDelay(50 / portTICK_PERIOD_MS);

    uint32_t pin_val = 0;
    ret |= esp_io_expander_get_level(ctx->io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
    ESP_LOGI(TAG, "IO expander initialized: %x", DRV_IO_EXP_OUTPUT_MASK | (uint16_t)pin_val);
    assert(ret == ESP_OK);
}

static void init_spi(sensecap_ctx_t *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Initialize SSCMA SPI bus");
    spi_bus_config_t spi_cfg = {0};
    spi_cfg.mosi_io_num = BSP_SPI2_HOST_MOSI;
    spi_cfg.miso_io_num = BSP_SPI2_HOST_MISO;
    spi_cfg.sclk_io_num = BSP_SPI2_HOST_SCLK;
    spi_cfg.quadwp_io_num = -1;
    spi_cfg.quadhd_io_num = -1;
    spi_cfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1;
    spi_cfg.max_transfer_sz = 4095;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Initialize QSPI bus");
    spi_bus_config_t qspi_cfg = {0};
    qspi_cfg.sclk_io_num = BSP_SPI3_HOST_PCLK;
    qspi_cfg.data0_io_num = BSP_SPI3_HOST_DATA0;
    qspi_cfg.data1_io_num = BSP_SPI3_HOST_DATA1;
    qspi_cfg.data2_io_num = BSP_SPI3_HOST_DATA2;
    qspi_cfg.data3_io_num = BSP_SPI3_HOST_DATA3;
    qspi_cfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * DRV_LCD_BITS_PER_PIXEL / 8 / CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &qspi_cfg, SPI_DMA_CH_AUTO));
}

static void spd2010_invalidate_area_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    area->x1 = (x1 >> 2) << 2;
    area->x2 = ((x2 >> 2) << 2) + 3;
}

static void init_display(sensecap_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BSP_LCD_SPI_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = DRV_LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = DRV_LCD_CMD_BITS,
        .lcd_param_bits = DRV_LCD_PARAM_BITS,
        .flags = { .quad_mode = true },
    };
    spd2010_vendor_config_t vendor_config = {
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &ctx->panel_io);

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_GPIO_RST,
        .rgb_ele_order = DRV_LCD_RGB_ELEMENT_ORDER,
        .bits_per_pixel = DRV_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    esp_lcd_new_panel_spd2010(ctx->panel_io, &panel_config, &ctx->panel);
    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    lv_display_add_event_cb(lv_display_get_default(), spd2010_invalidate_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);
}

static uint8_t btn_get_key_level(button_driver_t *button_driver)
{
    return !io_expander_get_level(s_instance, BSP_KNOB_BTN);
}

static void btn_single_click_cb(void *button_handle, void *usr_data)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)usr_data;
    power_save_timer_wake_up(ctx->pst);
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void btn_long_press_start_cb(void *button_handle, void *usr_data)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)usr_data;
    bool is_charging = (io_expander_get_level(ctx, BSP_PWR_VBUS_IN_DET) == 0);
    ctx->long_press_cnt = 0;
    if (is_charging) {
        ESP_LOGI(TAG, "charging");
    } else {
        io_expander_set_level(ctx, BSP_PWR_LCD, 0);
        io_expander_set_level(ctx, BSP_PWR_SYSTEM, 0);
    }
}

static void btn_long_press_hold_cb(void *button_handle, void *usr_data)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)usr_data;
    ctx->long_press_cnt++;
    if (ctx->long_press_cnt > 400) {
        ESP_LOGI(TAG, "Factory reset");
        nvs_flash_erase();
        esp_restart();
    }
}

static void init_button(sensecap_ctx_t *ctx)
{
    ESP_LOGI(TAG, "waiting for knob button release");
    while (io_expander_get_level(ctx, BSP_KNOB_BTN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    button_config_t btn_config = { .long_press_time = 2000, .short_press_time = 0 };
    ctx->btn_driver = calloc(1, sizeof(button_driver_t));
    ctx->btn_driver->enable_power_save = false;
    ctx->btn_driver->get_key_level = btn_get_key_level;

    ESP_ERROR_CHECK(iot_button_create(&btn_config, ctx->btn_driver, &ctx->btns));
    iot_button_register_cb(ctx->btns, BUTTON_SINGLE_CLICK, NULL, btn_single_click_cb, ctx);
    iot_button_register_cb(ctx->btns, BUTTON_LONG_PRESS_START, NULL, btn_long_press_start_cb, ctx);
    iot_button_register_cb(ctx->btns, BUTTON_LONG_PRESS_HOLD, NULL, btn_long_press_hold_cb, ctx);
}

static void init_knob(sensecap_ctx_t *ctx)
{
    ctx->knob = board_knob_create(BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
    board_knob_on_rotate(ctx->knob, on_knob_rotate, ctx);
    ESP_LOGI(TAG, "Knob initialized with pins A:%d B:%d", BSP_KNOB_A_PIN, BSP_KNOB_B_PIN);
}

static uint16_t battery_get_voltage(void)
{
    static bool initialized = false;
    static adc_oneshot_unit_handle_t adc_handle;
    static adc_cali_handle_t cali_handle = NULL;

    if (!initialized) {
        adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
        adc_oneshot_new_unit(&init_cfg, &adc_handle);
        adc_oneshot_chan_cfg_t ch_cfg = { .atten = BSP_BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
        adc_oneshot_config_channel(adc_handle, BSP_BAT_ADC_CHAN, &ch_cfg);
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1, .chan = BSP_BAT_ADC_CHAN,
            .atten = BSP_BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK)
            initialized = true;
    }
    if (initialized) {
        int raw_value = 0, voltage = 0;
        adc_oneshot_read(adc_handle, BSP_BAT_ADC_CHAN, &raw_value);
        adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage);
        voltage = voltage * 82 / 20;
        return (uint16_t)voltage;
    }
    return 0;
}

static uint8_t battery_get_percent(bool do_print)
{
    int voltage = 0;
    for (int i = 0; i < 10; i++)
        voltage += battery_get_voltage();
    voltage /= 10;
    int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    if (do_print) printf("voltage: %dmV, percentage: %d%%\r\n", voltage, percent);
    return (uint8_t)percent;
}

static int cmd_reboot(int argc, char **argv) { esp_restart(); return 0; }
static int cmd_factory_reset(void *context, int argc, char **argv) { nvs_flash_erase(); esp_restart(); return 0; }

static int cmd_shutdown(void *context, int argc, char **argv)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)context;
    if (ctx->backlight) backlight_set_brightness(ctx->backlight, 0, false);
    io_expander_set_level(ctx, BSP_PWR_SYSTEM, 0);
    return 0;
}

static int cmd_battery(void *context, int argc, char **argv)
{
    (void)context;
    battery_get_percent(true);
    return 0;
}

static int cmd_read_mac(void *context, int argc, char **argv)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("wifi_sta_mac: " MACSTR "\n", MAC2STR(mac));
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    printf("wifi_softap_mac: " MACSTR "\n", MAC2STR(mac));
    esp_read_mac(mac, ESP_MAC_BT);
    printf("bt_mac: " MACSTR "\n", MAC2STR(mac));
    return 0;
}

static int cmd_version(void *context, int argc, char **argv)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)context;
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *region = "UNKNOWN";
#if defined(CONFIG_LANGUAGE_ZH_CN)
    region = "CN";
#elif defined(CONFIG_LANGUAGE_EN_US)
    region = "US";
#elif defined(CONFIG_LANGUAGE_JA_JP)
    region = "JP";
#elif defined(CONFIG_LANGUAGE_ES_ES)
    region = "ES";
#elif defined(CONFIG_LANGUAGE_DE_DE)
    region = "DE";
#elif defined(CONFIG_LANGUAGE_FR_FR)
    region = "FR";
#elif defined(CONFIG_LANGUAGE_IT_IT)
    region = "IT";
#elif defined(CONFIG_LANGUAGE_PT_PT)
    region = "PT";
#elif defined(CONFIG_LANGUAGE_RU_RU)
    region = "RU";
#elif defined(CONFIG_LANGUAGE_KO_KR)
    region = "KR";
#endif
    printf("{\"type\":0,\"name\":\"VER?\",\"code\":0,\"data\":{\"software\":\"%s\",\"hardware\":\"watcher xiaozhi agent\",\"camera\":%d,\"region\":\"%s\"}}\n",
        app_desc->version, ctx->camera ? 1 : 0, region);
    return 0;
}

static void init_cmd(sensecap_ctx_t *ctx)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.max_cmdline_length = 1024;
    repl_config.prompt = "SenseCAP>";

    const esp_console_cmd_t cmd1 = { .command = "reboot", .help = "reboot the device", .func = cmd_reboot };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd1));

    const esp_console_cmd_t cmd2 = { .command = "shutdown", .help = "shutdown the device",
        .func_w_context = cmd_shutdown, .context = ctx };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd2));

    const esp_console_cmd_t cmd3 = { .command = "battery", .help = "get battery percent",
        .func_w_context = cmd_battery, .context = ctx };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd3));

    const esp_console_cmd_t cmd4 = { .command = "factory_reset", .help = "factory reset and reboot the device",
        .func_w_context = cmd_factory_reset, .context = ctx };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd4));

    const esp_console_cmd_t cmd5 = { .command = "read_mac", .help = "Read mac address",
        .func_w_context = cmd_read_mac, .context = ctx };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd5));

    const esp_console_cmd_t cmd6 = { .command = "version", .help = "Read version info",
        .func_w_context = cmd_version, .context = ctx };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd6));

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

static void init_camera(sensecap_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Initialize Camera");
    const gpio_config_t io_config = {
        .pin_bit_mask = (1ULL << BSP_SD_SPI_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_config) != ESP_OK) return;
    gpio_set_level(BSP_SD_SPI_CS, 1);
    ctx->camera = sscma_camera_create(ctx->io_exp_handle);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "sensecap-watcher";
}

static void *get_led(board_desc_t *self)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)self;
    if (!ctx->led) ctx->led = single_led_create(BUILTIN_LED_GPIO);
    return ctx->led;
}

static void *get_audio_codec(board_desc_t *self)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = sensecap_audio_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7243E_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((sensecap_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)self;
    if (!ctx->backlight)
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    return ctx->backlight;
}

static void *get_camera(board_desc_t *self)
{
    return ((sensecap_ctx_t *)self)->camera;
}

static bool get_battery_level(board_desc_t *self, int *level, bool *charging, bool *discharging)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)self;
    static bool last_discharging = false;
    *charging = (io_expander_get_level(ctx, BSP_PWR_VBUS_IN_DET) == 0);
    *discharging = !(*charging);
    *level = (int)battery_get_percent(false);

    if (*discharging != last_discharging) {
        power_save_timer_set_enabled(ctx->pst, *discharging);
        last_discharging = *discharging;
    }
    if (*level <= 1 && *discharging) {
        ESP_LOGI(TAG, "Battery level is low, shutting down");
        io_expander_set_level(ctx, BSP_PWR_SYSTEM, 0);
    }
    return true;
}

static void destroy(board_desc_t *self)
{
    sensecap_ctx_t *ctx = (sensecap_ctx_t *)self;
    if (ctx->knob) board_knob_delete(ctx->knob);
    if (ctx->pst) power_save_timer_destroy(ctx->pst);
    if (ctx->camera) sscma_camera_destroy(ctx->camera);
    free(ctx->btn_driver);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    sensecap_ctx_t *ctx = calloc(1, sizeof(sensecap_ctx_t));
    if (!ctx) return NULL;
    s_instance = ctx;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_led = get_led;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.get_camera = get_camera;
    ctx->base.get_battery_level = get_battery_level;
    ctx->base.destroy = destroy;

    ESP_LOGI(TAG, "Initialize Sensecap Watcher");
    init_power_save(ctx);
    init_i2c(ctx);
    init_spi(ctx);
    init_expander(ctx);
    init_cmd(ctx);
    init_button(ctx);
    init_knob(ctx);
    init_display(ctx);

    backlight_t *bl = (backlight_t *)get_backlight(&ctx->base);
    backlight_restore_brightness(bl);

    init_camera(ctx);

    return &ctx->base;
}

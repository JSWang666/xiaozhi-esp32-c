#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "display/display.h"
#include "backlight.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>

#include "esp_io_expander_tca9554.h"
#include "esp_lcd_axs15231b.h"
#include "lvgl.h"
#include "c_api/board_c_api.h"

display_t *custom_lcd_349_display_create(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy);

#define TAG "waveshare_lcd_3_49"

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;

    board_btn_t *boot_button;
    board_btn_t *pwr_button;

    i2c_master_dev_handle_t touch_dev;
    lv_indev_t *touch_indev;
    bool pwr_control_en;
} lcd349_ctx_t;

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) {
        board_enter_wifi_config_mode(board_get_instance());
        return;
    }
    app_toggle_chat(app);
}

static void on_pwr_long_press(void *ud)
{
    lcd349_ctx_t *ctx = (lcd349_ctx_t *)ud;
    if (ctx->pwr_control_en) {
        ctx->pwr_control_en = false;
        esp_io_expander_set_level(ctx->io_expander, IO_EXPANDER_PIN_NUM_6, 0);
    }
}

static void on_pwr_press_up(void *ud)
{
    lcd349_ctx_t *ctx = (lcd349_ctx_t *)ud;
    if (!ctx->pwr_control_en) {
        ctx->pwr_control_en = true;
    }
}

static void init_i2c(lcd349_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_tca9554(lcd349_ctx_t *ctx)
{
    esp_err_t ret = esp_io_expander_new_i2c_tca9554(ctx->i2c_bus,
        ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &ctx->io_expander);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "TCA9554 create returned error");
    ret = esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);
    ESP_ERROR_CHECK(ret);
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, 1);
    ESP_ERROR_CHECK(ret);
}

static void init_spi(void)
{
    ESP_LOGI(TAG, "Initialize QSPI bus");
    spi_bus_config_t buscfg = {
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .sclk_io_num = LCD_PCLK,
        .max_transfer_sz = LVGL_DMA_BUFF_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(lcd349_ctx_t *ctx)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = ((uint64_t)0x01 << LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &ctx->panel_io));

    ESP_LOGI(TAG, "Install LCD driver");
    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    esp_lcd_new_panel_axs15231b(ctx->panel_io, &panel_config, &ctx->panel);

    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_lcd_panel_init(ctx->panel);

    ctx->display = custom_lcd_349_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(lcd349_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t pwr_cfg = { .gpio_num = PWR_BUTTON_GPIO };
    ctx->pwr_button = board_btn_create_gpio(&pwr_cfg);
    board_btn_on_long_press(ctx->pwr_button, on_pwr_long_press, ctx);
    board_btn_on_press_up(ctx->pwr_button, on_pwr_press_up, ctx);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    i2c_master_dev_handle_t i2c_dev = (i2c_master_dev_handle_t)lv_indev_get_user_data(indev);
    uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buf[32] = {0};
    i2c_master_transmit_receive(i2c_dev, cmd, 11, buf, 32, 1000);
    uint16_t pointX = (((uint16_t)buf[2] & 0x0f) << 8) | (uint16_t)buf[3];
    uint16_t pointY = (((uint16_t)buf[4] & 0x0f) << 8) | (uint16_t)buf[5];
    if (buf[1] > 0 && buf[1] < 5) {
        data->state = LV_INDEV_STATE_PRESSED;
        if (pointX > DISPLAY_WIDTH) pointX = DISPLAY_WIDTH;
        if (pointY > DISPLAY_HEIGHT) pointY = DISPLAY_HEIGHT;
        data->point.x = pointY;
        data->point.y = (DISPLAY_HEIGHT - pointX);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void init_touch(lcd349_ctx_t *ctx)
{
    i2c_master_bus_handle_t touch_i2c_bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = I2C_Touch_SDA_PIN,
        .scl_io_num = I2C_Touch_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &touch_i2c_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_Touch_ADDRESS,
        .scl_speed_hz = 300000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(touch_i2c_bus, &dev_cfg, &ctx->touch_dev));

    ctx->touch_indev = lv_indev_create();
    lv_indev_set_type(ctx->touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx->touch_indev, touch_read_cb);
    lv_indev_set_user_data(ctx->touch_indev, ctx->touch_dev);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "WaveshareEsp32s3TouchLCD3inch49";
}

static void *get_audio_codec(board_desc_t *self)
{
    lcd349_ctx_t *ctx = (lcd349_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = box_codec_create(ctx->i2c_bus,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    lcd349_ctx_t *ctx = (lcd349_ctx_t *)self;
    return ctx->display;
}

static void *get_backlight(board_desc_t *self)
{
    lcd349_ctx_t *ctx = (lcd349_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void board_destroy(board_desc_t *self)
{
    lcd349_ctx_t *ctx = (lcd349_ctx_t *)self;
    if (ctx->backlight) {
        backlight_destroy(ctx->backlight);
        ctx->backlight = NULL;
    }
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->pwr_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    lcd349_ctx_t *ctx = calloc(1, sizeof(lcd349_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = board_destroy;

    init_i2c(ctx);
    init_tca9554(ctx);
    init_spi();
    init_lcd_display(ctx);
    init_buttons(ctx);
    init_touch(ctx);

    if (gpio_get_level(PWR_BUTTON_GPIO)) {
        ctx->pwr_control_en = true;
    }

    ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    backlight_restore_brightness(ctx->backlight);

    return &ctx->base;
}

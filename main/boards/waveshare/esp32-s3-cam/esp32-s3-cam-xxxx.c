#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "device_state.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_io_spi.h>
#include "custom_io_expander_ch32v003.h"
#include <esp_ota_ops.h>
#ifdef CONFIG_BSP_LCD_SIZE_3_5INCH
#include "esp_lcd_st7796.h"
#include "c_api/board_c_api.h"
#endif

#define TAG "waveshare_s3_cam_xxxx"

static void switch_to_main(void)
{
    const esp_partition_t *factory_part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory_part) {
        ESP_LOGE("APP_SWITCH", "Factory partition not found");
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(factory_part);
    if (err == ESP_OK) {
        ESP_LOGI("APP_SWITCH", "Set factory as boot partition, restarting");
        esp_restart();
    } else {
        ESP_LOGE("APP_SWITCH", "Failed to set boot partition: %s", esp_err_to_name(err));
    }
}

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t i2c_bus;
    esp_io_expander_handle_t io_expander;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;

    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
} wscam_ctx_t;

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

static void on_boot_long_press(void *ud)
{
    (void)ud;
    switch_to_main();
}

static void init_i2c(wscam_ctx_t *ctx)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 1,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &ctx->i2c_bus));
}

static void init_expander(wscam_ctx_t *ctx)
{
    custom_io_expander_new_i2c_ch32v003(ctx->i2c_bus, BSP_IO_EXPANDER_I2C_ADDRESS, &ctx->io_expander);
    esp_io_expander_set_dir(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 |
        IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_6,
        IO_EXPANDER_OUTPUT);

    esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(ctx->io_expander,
        IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_6, 1);
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

#ifndef CONFIG_BSP_LCD_SIZE_3_5INCH
static void init_st7789_display(wscam_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, DISPLAY_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}
#endif

#ifdef CONFIG_BSP_LCD_SIZE_3_5INCH
static void init_st7796_display(wscam_ctx_t *ctx)
{
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &ctx->panel_io));

    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(ctx->panel_io, &panel_config, &ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(ctx->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(ctx->panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(ctx->panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(ctx->panel, DISPLAY_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(ctx->panel, true));

    ctx->display = spi_lcd_display_create(ctx->panel_io, ctx->panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}
#endif

static void init_buttons(wscam_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
    board_btn_on_long_press(ctx->boot_button, on_boot_long_press, ctx);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *get_audio_codec(board_desc_t *self)
{
    wscam_ctx_t *ctx = (wscam_ctx_t *)self;
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
    wscam_ctx_t *ctx = (wscam_ctx_t *)self;
    return ctx->display;
}

static void destroy(board_desc_t *self)
{
    wscam_ctx_t *ctx = (wscam_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    wscam_ctx_t *ctx = calloc(1, sizeof(wscam_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_expander(ctx);
    init_spi();
    init_buttons(ctx);

#ifdef CONFIG_BSP_LCD_SIZE_3_5INCH
    init_st7796_display(ctx);
#else
    init_st7789_display(ctx);
#endif

    return &ctx->base;
}

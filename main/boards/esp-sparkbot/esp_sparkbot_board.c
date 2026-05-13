#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/codec_c_api.h"
#include "c_api/display_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "display/display.h"
#include "backlight.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include "c_api/board_c_api.h"

#define TAG "esp_sparkbot"

typedef struct {
    board_desc_t base;
    i2c_master_bus_handle_t i2c_bus;
    audio_codec_t *codec;
    display_t *display;
    backlight_t *backlight;
    board_btn_t *boot_button;
} sparkbot_ctx_t;

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

static void init_i2c(sparkbot_ctx_t *ctx)
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

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_CLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_display(sparkbot_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_GPIO,
        .dc_gpio_num = DISPLAY_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}

static void init_buttons(sparkbot_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);
}

static void send_uart_message(const char *command_str)
{
    uint8_t len = strlen(command_str);
    uart_write_bytes(ECHO_UART_PORT_NUM, command_str, len);
    ESP_LOGI(TAG, "Sent command: %s", command_str);
}

static void init_echo_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));
    send_uart_message("w2");
}

static mcp_tool_result_t tool_go_forward(const void *args, void *ud)
{
    (void)args; (void)ud;
    send_uart_message("x0.0 y1.0");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t tool_go_back(const void *args, void *ud)
{
    (void)args; (void)ud;
    send_uart_message("x0.0 y-1.0");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t tool_turn_left(const void *args, void *ud)
{
    (void)args; (void)ud;
    send_uart_message("x-1.0 y0.0");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t tool_turn_right(const void *args, void *ud)
{
    (void)args; (void)ud;
    send_uart_message("x1.0 y0.0");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t tool_dance(const void *args, void *ud)
{
    (void)args; (void)ud;
    send_uart_message("d1");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static void init_tools(void)
{
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    mcp_server_add_tool_c(mcp, "self.chassis.go_forward",
        "前进", NULL, 0, tool_go_forward, NULL);
    mcp_server_add_tool_c(mcp, "self.chassis.go_back",
        "后退", NULL, 0, tool_go_back, NULL);
    mcp_server_add_tool_c(mcp, "self.chassis.turn_left",
        "向左转", NULL, 0, tool_turn_left, NULL);
    mcp_server_add_tool_c(mcp, "self.chassis.turn_right",
        "向右转", NULL, 0, tool_turn_right, NULL);
    mcp_server_add_tool_c(mcp, "self.chassis.dance",
        "跳舞", NULL, 0, tool_dance, NULL);
}

static const char *get_board_type(board_desc_t *self)
{
    (void)self;
    return "esp-sparkbot";
}

static void *get_audio_codec(board_desc_t *self)
{
    sparkbot_ctx_t *ctx = (sparkbot_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(ctx->i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, false);
    }
    return ctx->codec;
}

static void *get_display(board_desc_t *self)
{
    return ((sparkbot_ctx_t *)self)->display;
}

static void *get_backlight(board_desc_t *self)
{
    sparkbot_ctx_t *ctx = (sparkbot_ctx_t *)self;
    if (!ctx->backlight) {
        ctx->backlight = pwm_backlight_create(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 25000);
    }
    return ctx->backlight;
}

static void destroy(board_desc_t *self)
{
    sparkbot_ctx_t *ctx = (sparkbot_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    sparkbot_ctx_t *ctx = calloc(1, sizeof(sparkbot_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = get_board_type;
    ctx->base.get_audio_codec = get_audio_codec;
    ctx->base.get_display = get_display;
    ctx->base.get_backlight = get_backlight;
    ctx->base.destroy = destroy;

    init_i2c(ctx);
    init_spi();
    init_display(ctx);
    init_buttons(ctx);
    init_echo_uart();
    init_tools();
    backlight_restore_brightness(get_backlight(&ctx->base));

    return &ctx->base;
}

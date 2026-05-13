#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/codec_c_api.h"
#include "c_api/app_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "led/single_led.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "display/display.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <led_strip.h>
#include <driver/rmt_tx.h>
#include "c_api/settings_c_api.h"

#define TAG "KevinBoxBoard"

typedef struct {
    board_desc_t base;

    i2c_master_bus_handle_t codec_i2c_bus;
    led_t *led;
    audio_codec_t *codec;

    board_btn_t *boot_button;

    led_strip_handle_t led_strip;
    int brightness_level;
    int max_leds;
} kevin_c3_ctx_t;

static int level_to_brightness(int level)
{
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    return (1 << level) - 1;
}

static void on_boot_press_down(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_start_listening(app);
}

static void on_boot_press_up(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (app) app_stop_listening(app);
}

static void init_codec_i2c(kevin_c3_ctx_t *ctx)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &ctx->codec_i2c_bus));

    if (i2c_master_probe(ctx->codec_i2c_bus, 0x18, 1000) != ESP_OK) {
        while (true) {
            ESP_LOGE(TAG, "Failed to probe I2C bus, please check if you have installed the correct firmware");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }
}

static void init_buttons(kevin_c3_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_press_down(ctx->boot_button, on_boot_press_down, ctx);
    board_btn_on_press_up(ctx->boot_button, on_boot_press_up, ctx);
}

static void init_led_strip(kevin_c3_ctx_t *ctx)
{
    ctx->max_leds = 8;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BUILTIN_LED_GPIO,
        .max_leds = ctx->max_leds,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &ctx->led_strip));
    led_strip_clear(ctx->led_strip);

    ctx->brightness_level = 4;
}

static mcp_tool_result_t strip_get_brightness(const void *args, void *ud)
{
    kevin_c3_ctx_t *ctx = (kevin_c3_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", ctx->brightness_level);
    res.text = strdup(buf);
    return res;
}

static mcp_tool_result_t strip_set_brightness(const void *args, void *ud)
{
    kevin_c3_ctx_t *ctx = (kevin_c3_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    (void)args;
    return res;
}

static mcp_tool_result_t strip_set_all_color(const void *args, void *ud)
{
    kevin_c3_ctx_t *ctx = (kevin_c3_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    (void)args;
    return res;
}

static void init_strip_tools(kevin_c3_ctx_t *ctx)
{
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

    mcp_server_add_tool_c(mcp, "self.led_strip.get_brightness",
        "Get the brightness of the led strip (0-8)",
        NULL, 0, strip_get_brightness, ctx);

    static const mcp_tool_param_t bright_params[] = {
        { "level", MCP_PARAM_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.set_brightness",
        "Set the brightness of the led strip (0-8)",
        bright_params, 1, strip_set_brightness, ctx);

    static const mcp_tool_param_t color_params[] = {
        { "red", MCP_PARAM_TYPE_INTEGER },
        { "green", MCP_PARAM_TYPE_INTEGER },
        { "blue", MCP_PARAM_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.set_all_color",
        "Set the color of all leds.",
        color_params, 3, strip_set_all_color, ctx);
}

static const char *kb_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *kb_get_led(board_desc_t *self)
{
    kevin_c3_ctx_t *ctx = (kevin_c3_ctx_t *)self;
    if (!ctx->led) {
        ctx->led = single_led_create(BUILTIN_LED_GPIO);
    }
    return ctx->led;
}

static void *kb_get_audio_codec(board_desc_t *self)
{
    kevin_c3_ctx_t *ctx = (kevin_c3_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = es8311_codec_create(
            ctx->codec_i2c_bus, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            false, false);
    }
    return ctx->codec;
}

static void kb_destroy(board_desc_t *self)
{
    kevin_c3_ctx_t *ctx = (kevin_c3_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    kevin_c3_ctx_t *ctx = calloc(1, sizeof(kevin_c3_ctx_t));
    if (!ctx) return NULL;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = kb_get_board_type;
    ctx->base.get_led = kb_get_led;
    ctx->base.get_audio_codec = kb_get_audio_codec;
    ctx->base.destroy = kb_destroy;

    init_codec_i2c(ctx);
    init_buttons(ctx);
    init_led_strip(ctx);
    init_strip_tools(ctx);

    esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);

    return &ctx->base;
}

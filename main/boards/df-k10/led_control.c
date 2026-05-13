#include "c_api/mcp_server_c_api.h"

#include <esp_log.h>
#include <led_strip.h>
#include <driver/rmt_tx.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"

#define TAG "LedStripControl"
#define K10_LED_COUNT 3

typedef struct {
    led_strip_handle_t strip;
    int brightness_level;
} k10_led_ctx_t;

static k10_led_ctx_t *g_led_ctx = NULL;

static int level_to_brightness(int level)
{
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    return (1 << level) - 1;
}

static void apply_color(k10_led_ctx_t *ctx, int idx, uint8_t r, uint8_t g, uint8_t b)
{
    int br = level_to_brightness(ctx->brightness_level);
    uint8_t ar = (uint8_t)((int)r * br / 255);
    uint8_t ag = (uint8_t)((int)g * br / 255);
    uint8_t ab = (uint8_t)((int)b * br / 255);
    led_strip_set_pixel(ctx->strip, idx, ar, ag, ab);
}

static mcp_tool_result_t led_get_brightness(const void *args, void *ud)
{
    k10_led_ctx_t *ctx = (k10_led_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", ctx->brightness_level);
    res.text = strdup(buf);
    return res;
}

static mcp_tool_result_t led_set_brightness(const void *args, void *ud)
{
    k10_led_ctx_t *ctx = (k10_led_ctx_t *)ud;
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    int level = mcp_property_get_int(props, "level");
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    ctx->brightness_level = level;
    ESP_LOGI(TAG, "Set brightness level to %d", level);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t led_set_single_color(const void *args, void *ud)
{
    k10_led_ctx_t *ctx = (k10_led_ctx_t *)ud;
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    int index = mcp_property_get_int(props, "index");
    int red = mcp_property_get_int(props, "red");
    int green = mcp_property_get_int(props, "green");
    int blue = mcp_property_get_int(props, "blue");
    if (index >= 0 && index < K10_LED_COUNT) {
        apply_color(ctx, index, (uint8_t)red, (uint8_t)green, (uint8_t)blue);
        led_strip_refresh(ctx->strip);
    }
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t led_set_all_color(const void *args, void *ud)
{
    k10_led_ctx_t *ctx = (k10_led_ctx_t *)ud;
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    int red = mcp_property_get_int(props, "red");
    int green = mcp_property_get_int(props, "green");
    int blue = mcp_property_get_int(props, "blue");
    for (int i = 0; i < K10_LED_COUNT; i++)
        apply_color(ctx, i, (uint8_t)red, (uint8_t)green, (uint8_t)blue);
    led_strip_refresh(ctx->strip);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t led_blink(const void *args, void *ud)
{
    k10_led_ctx_t *ctx = (k10_led_ctx_t *)ud;
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    int red = mcp_property_get_int(props, "red");
    int green = mcp_property_get_int(props, "green");
    int blue = mcp_property_get_int(props, "blue");
    for (int i = 0; i < K10_LED_COUNT; i++)
        apply_color(ctx, i, (uint8_t)red, (uint8_t)green, (uint8_t)blue);
    led_strip_refresh(ctx->strip);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t led_scroll(const void *args, void *ud)
{
    k10_led_ctx_t *ctx = (k10_led_ctx_t *)ud;
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    int red = mcp_property_get_int(props, "red");
    int green = mcp_property_get_int(props, "green");
    int blue = mcp_property_get_int(props, "blue");
    for (int i = 0; i < K10_LED_COUNT; i++)
        apply_color(ctx, i, (uint8_t)red, (uint8_t)green, (uint8_t)blue);
    led_strip_refresh(ctx->strip);
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

led_strip_handle_t k10_led_control_init(void)
{
    k10_led_ctx_t *ctx = calloc(1, sizeof(k10_led_ctx_t));
    if (!ctx) return NULL;
    ctx->brightness_level = 4;
    g_led_ctx = ctx;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BUILTIN_LED_GPIO,
        .max_leds = K10_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &ctx->strip));
    led_strip_clear(ctx->strip);

    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return ctx->strip;

    mcp_server_add_tool_c(mcp, "self.led_strip.get_brightness",
        "Get LED strip brightness (0-8)", NULL, 0, led_get_brightness, ctx);

    mcp_property_desc_t br_props[] = {
        { .name = "level", .type = MCP_PROPERTY_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.set_brightness",
        "Set LED strip brightness (0-8)", br_props, 1, led_set_brightness, ctx);

    mcp_property_desc_t single_props[] = {
        { .name = "index", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "red", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "green", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "blue", .type = MCP_PROPERTY_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.set_single_color",
        "Set single LED color", single_props, 4, led_set_single_color, ctx);

    mcp_property_desc_t all_props[] = {
        { .name = "red", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "green", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "blue", .type = MCP_PROPERTY_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.set_all_color",
        "Set all LEDs color", all_props, 3, led_set_all_color, ctx);

    mcp_property_desc_t blink_props[] = {
        { .name = "red", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "green", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "blue", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "interval", .type = MCP_PROPERTY_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.blink",
        "Blink LED strip", blink_props, 4, led_blink, ctx);

    mcp_property_desc_t scroll_props[] = {
        { .name = "red", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "green", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "blue", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "length", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "interval", .type = MCP_PROPERTY_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.led_strip.scroll",
        "Scroll LED strip", scroll_props, 5, led_scroll, ctx);

    return ctx->strip;
}

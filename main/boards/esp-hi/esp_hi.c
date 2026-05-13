#include "board_defs.h"
#include "button.h"
#include "config.h"
#include "c_api/app_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "c_api/display_c_api.h"
#include "assets/lang_c.h"
#include "device_state.h"
#include "display/display.h"
#include "audio/audio_codec.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include "esp_lcd_ili9341.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_CONSOLE_NONE
#include "servo_dog_ctrl.h"
#endif

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
#include "esp_hi_web_control.h"
#include <esp_wifi.h>
#include <esp_event.h>
#endif

#define TAG "ESP_HI"

audio_codec_t *adc_pdm_audio_codec_create(
    int input_sample_rate, int output_sample_rate,
    uint32_t adc_mic_channel, int pdm_speak_p, int pdm_speak_n, int pa_ctl);

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, NULL, 0, 120},
    {0xB1, (uint8_t []){0x05, 0x3A, 0x3A}, 3, 0},
    {0xB2, (uint8_t []){0x05, 0x3A, 0x3A}, 3, 0},
    {0xB3, (uint8_t []){0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A}, 6, 0},
    {0xB4, (uint8_t []){0x03}, 1, 0},
    {0xC0, (uint8_t []){0x44, 0x04, 0x04}, 3, 0},
    {0xC1, (uint8_t []){0xC0}, 1, 0},
    {0xC2, (uint8_t []){0x0D, 0x00}, 2, 0},
    {0xC3, (uint8_t []){0x8D, 0x6A}, 2, 0},
    {0xC4, (uint8_t []){0x8D, 0xEE}, 2, 0},
    {0xC5, (uint8_t []){0x08}, 1, 0},
    {0xE0, (uint8_t []){0x0F, 0x10, 0x03, 0x03, 0x07, 0x02, 0x00, 0x02,
                         0x07, 0x0C, 0x13, 0x38, 0x0A, 0x0E, 0x03, 0x10}, 16, 0},
    {0xE1, (uint8_t []){0x10, 0x0B, 0x04, 0x04, 0x10, 0x03, 0x00, 0x03,
                         0x03, 0x09, 0x17, 0x33, 0x0B, 0x0C, 0x06, 0x10}, 16, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x05}, 1, 0},
    {0x36, (uint8_t []){0xC8}, 1, 0},
    {0x29, NULL, 0, 0},
    {0x2C, NULL, 0, 0},
};

typedef struct {
    board_desc_t base;

    audio_codec_t *codec;
    display_t *display;

    board_btn_t *boot_button;
    board_btn_t *audio_wake_button;
    board_btn_t *move_wake_button;

    led_strip_handle_t led_strip;
    bool led_on;

    int64_t last_trigger_time;
    int gesture_state;

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    bool web_server_initialized;
#endif
} esp_hi_ctx_t;

static esp_hi_ctx_t *g_ctx = NULL;

static esp_err_t set_led_color(esp_hi_ctx_t *ctx, uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < 4; i++)
        ret |= led_strip_set_pixel(ctx->led_strip, i, r, g, b);
    ret |= led_strip_refresh(ctx->led_strip);
    return ret;
}

static void handle_move_wake_press_down(esp_hi_ctx_t *ctx)
{
    int64_t current_time = esp_timer_get_time() / 1000;
    int64_t interval = ctx->last_trigger_time == 0 ? 0 : current_time - ctx->last_trigger_time;
    ctx->last_trigger_time = current_time;

    if (interval > 1000) {
        ctx->gesture_state = 0;
    } else {
        switch (ctx->gesture_state) {
        case 0: break;
        case 1:
            if (interval > 300) ctx->gesture_state = 2;
            break;
        case 2:
            if (interval > 100) ctx->gesture_state = 0;
            break;
        }
    }
}

static void handle_move_wake_press_up(esp_hi_ctx_t *ctx)
{
    int64_t current_time = esp_timer_get_time() / 1000;
    int64_t interval = current_time - ctx->last_trigger_time;

    if (interval > 1000) {
        ctx->gesture_state = 0;
    } else {
        switch (ctx->gesture_state) {
        case 0:
            if (interval > 300) ctx->gesture_state = 1;
            break;
        case 1: break;
        case 2:
            if (interval < 100) {
                ESP_LOGI(TAG, "gesture detected");
                ctx->gesture_state = 0;
                app_context_t *app = app_get_context();
                if (app) app_toggle_chat(app);
            }
            break;
        }
    }
}

static void on_boot_click(void *ud)
{
    (void)ud;
    app_context_t *app = app_get_context();
    if (!app) return;
    if (app_get_device_state(app) == kDeviceStateStarting) return;
    app_toggle_chat(app);
}

static void on_move_press_down(void *ud)
{
    handle_move_wake_press_down((esp_hi_ctx_t *)ud);
}

static void on_move_press_up(void *ud)
{
    handle_move_wake_press_up((esp_hi_ctx_t *)ud);
}

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
static void web_server_init_task(void *arg)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (!ctx->web_server_initialized) {
        ESP_LOGI(TAG, "WiFi connected, init web control server");
        esp_err_t err = esp_hi_web_control_server_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize web control server: %d", err);
        } else {
            ESP_LOGI(TAG, "Web control server initialized");
            ctx->web_server_initialized = true;
        }
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        xTaskCreate(web_server_init_task, "web_server_init", 1024 * 10, arg, 5, NULL);
    }
}
#endif

static void init_buttons(esp_hi_ctx_t *ctx)
{
    board_btn_gpio_cfg_t boot_cfg = { .gpio_num = BOOT_BUTTON_GPIO };
    ctx->boot_button = board_btn_create_gpio(&boot_cfg);
    board_btn_on_click(ctx->boot_button, on_boot_click, ctx);

    board_btn_gpio_cfg_t audio_cfg = { .gpio_num = AUDIO_WAKE_BUTTON_GPIO };
    ctx->audio_wake_button = board_btn_create_gpio(&audio_cfg);

    board_btn_gpio_cfg_t move_cfg = { .gpio_num = MOVE_WAKE_BUTTON_GPIO };
    ctx->move_wake_button = board_btn_create_gpio(&move_cfg);
    board_btn_on_press_down(ctx->move_wake_button, on_move_press_down, ctx);
    board_btn_on_press_up(ctx->move_wake_button, on_move_press_up, ctx);
}

static void init_led(esp_hi_ctx_t *ctx)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = GPIO_NUM_8,
        .max_leds = 4,
        .led_model = LED_MODEL_WS2812,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &ctx->led_strip));
    set_led_color(ctx, 0, 0, 0);
}

static void init_spi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_CLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * 10 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

static void init_lcd_display(esp_hi_ctx_t *ctx)
{
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_CS_PIN,
        .dc_gpio_num = DISPLAY_DC_PIN,
        .spi_mode = DISPLAY_SPI_MODE,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_RST_PIN,
        .rgb_ele_order = DISPLAY_RGB_ORDER,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_invert_color(panel, false);
    esp_lcd_panel_set_gap(panel, 0, 24);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_disp_on_off(panel, true);

    ctx->display = spi_lcd_display_create(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

#ifdef CONFIG_ESP_CONSOLE_NONE
    servo_dog_ctrl_config_t dog_cfg = {
        .fl_gpio_num = FL_GPIO_NUM,
        .fr_gpio_num = FR_GPIO_NUM,
        .bl_gpio_num = BL_GPIO_NUM,
        .br_gpio_num = BR_GPIO_NUM,
    };
    servo_dog_ctrl_init(&dog_cfg);
#endif
}

static mcp_tool_result_t light_get_power(const void *args, void *ud)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)ud;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    res.text = strdup(ctx->led_on ? "true" : "false");
    return res;
}

static mcp_tool_result_t light_turn_on(const void *args, void *ud)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)ud;
    set_led_color(ctx, 0xFF, 0xFF, 0xFF);
    ctx->led_on = true;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t light_turn_off(const void *args, void *ud)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)ud;
    set_led_color(ctx, 0, 0, 0);
    ctx->led_on = false;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

static mcp_tool_result_t light_set_rgb(const void *args, void *ud)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)ud;
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    int r = mcp_property_get_int(props, "r");
    int g = mcp_property_get_int(props, "g");
    int b = mcp_property_get_int(props, "b");
    set_led_color(ctx, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    ctx->led_on = true;
    mcp_tool_result_t res = { .is_error = false, .text = NULL };
    return res;
}

#ifdef CONFIG_ESP_CONSOLE_NONE
static mcp_tool_result_t dog_basic_control(const void *args, void *ud)
{
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    const char *action = mcp_property_get_string(props, "action");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    if (strcmp(action, "forward") == 0) {
        servo_dog_ctrl_send(DOG_STATE_FORWARD, NULL);
    } else if (strcmp(action, "backward") == 0) {
        servo_dog_ctrl_send(DOG_STATE_BACKWARD, NULL);
    } else if (strcmp(action, "turn_left") == 0) {
        servo_dog_ctrl_send(DOG_STATE_TURN_LEFT, NULL);
    } else if (strcmp(action, "turn_right") == 0) {
        servo_dog_ctrl_send(DOG_STATE_TURN_RIGHT, NULL);
    } else if (strcmp(action, "stop") == 0) {
        servo_dog_ctrl_send(DOG_STATE_IDLE, NULL);
    } else {
        res.is_error = true;
        res.text = strdup("Unknown action");
    }
    return res;
}

static mcp_tool_result_t dog_advanced_control(const void *args, void *ud)
{
    const mcp_property_list_t *props = (const mcp_property_list_t *)args;
    const char *action = mcp_property_get_string(props, "action");
    mcp_tool_result_t res = { .is_error = false, .text = NULL };

    if (strcmp(action, "sway_back_forth") == 0) {
        servo_dog_ctrl_send(DOG_STATE_SWAY_BACK_FORTH, NULL);
    } else if (strcmp(action, "lay_down") == 0) {
        servo_dog_ctrl_send(DOG_STATE_LAY_DOWN, NULL);
    } else if (strcmp(action, "sway") == 0) {
        dog_action_args_t dog_args = { .repeat_count = 4 };
        servo_dog_ctrl_send(DOG_STATE_SWAY, &dog_args);
    } else if (strcmp(action, "retract_legs") == 0) {
        servo_dog_ctrl_send(DOG_STATE_RETRACT_LEGS, NULL);
    } else if (strcmp(action, "shake_hand") == 0) {
        servo_dog_ctrl_send(DOG_STATE_SHAKE_HAND, NULL);
    } else if (strcmp(action, "shake_back_legs") == 0) {
        servo_dog_ctrl_send(DOG_STATE_SHAKE_BACK_LEGS, NULL);
    } else if (strcmp(action, "jump_forward") == 0) {
        servo_dog_ctrl_send(DOG_STATE_JUMP_FORWARD, NULL);
    } else {
        res.is_error = true;
        res.text = strdup("Unknown action");
    }
    return res;
}
#endif

static void init_tools(esp_hi_ctx_t *ctx)
{
    mcp_server_handle_t *mcp = mcp_server_get_instance();
    if (!mcp) return;

#ifdef CONFIG_ESP_CONSOLE_NONE
    mcp_property_desc_t basic_props[] = {
        { .name = "action", .type = MCP_PROPERTY_TYPE_STRING },
    };
    mcp_server_add_tool_c(mcp, "self.dog.basic_control",
        "\xe6\x9c\xba\xe5\x99\xa8\xe4\xba\xba\xe7\x9a\x84\xe5\x9f\xba\xe7\xa1\x80\xe5\x8a\xa8\xe4\xbd\x9c\xe3\x80\x82"
        "forward/backward/turn_left/turn_right/stop",
        basic_props, 1, dog_basic_control, ctx);

    mcp_property_desc_t adv_props[] = {
        { .name = "action", .type = MCP_PROPERTY_TYPE_STRING },
    };
    mcp_server_add_tool_c(mcp, "self.dog.advanced_control",
        "\xe6\x9c\xba\xe5\x99\xa8\xe4\xba\xba\xe7\x9a\x84\xe6\x89\xa9\xe5\xb1\x95\xe5\x8a\xa8\xe4\xbd\x9c\xe3\x80\x82"
        "sway_back_forth/lay_down/sway/retract_legs/shake_hand/shake_back_legs/jump_forward",
        adv_props, 1, dog_advanced_control, ctx);
#endif

    mcp_server_add_tool_c(mcp, "self.light.get_power",
        "Get light power state", NULL, 0, light_get_power, ctx);
    mcp_server_add_tool_c(mcp, "self.light.turn_on",
        "Turn on light", NULL, 0, light_turn_on, ctx);
    mcp_server_add_tool_c(mcp, "self.light.turn_off",
        "Turn off light", NULL, 0, light_turn_off, ctx);

    mcp_property_desc_t rgb_props[] = {
        { .name = "r", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "g", .type = MCP_PROPERTY_TYPE_INTEGER },
        { .name = "b", .type = MCP_PROPERTY_TYPE_INTEGER },
    };
    mcp_server_add_tool_c(mcp, "self.light.set_rgb",
        "Set RGB color", rgb_props, 3, light_set_rgb, ctx);
}

static const char *eh_get_board_type(board_desc_t *self)
{
    (void)self;
    return BOARD_TYPE;
}

static void *eh_get_audio_codec(board_desc_t *self)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)self;
    if (!ctx->codec) {
        ctx->codec = adc_pdm_audio_codec_create(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_ADC_MIC_CHANNEL,
            AUDIO_PDM_SPEAK_P_GPIO, AUDIO_PDM_SPEAK_N_GPIO,
            AUDIO_PA_CTL_GPIO);
    }
    return ctx->codec;
}

static void *eh_get_display(board_desc_t *self)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)self;
    return ctx->display;
}

static void eh_destroy(board_desc_t *self)
{
    esp_hi_ctx_t *ctx = (esp_hi_ctx_t *)self;
    board_btn_delete(ctx->boot_button);
    board_btn_delete(ctx->audio_wake_button);
    board_btn_delete(ctx->move_wake_button);
    free(ctx);
}

board_desc_t *create_board_desc(void)
{
    esp_hi_ctx_t *ctx = calloc(1, sizeof(esp_hi_ctx_t));
    if (!ctx) return NULL;
    g_ctx = ctx;

    ctx->base.kind = BOARD_KIND_WIFI;
    ctx->base.get_board_type = eh_get_board_type;
    ctx->base.get_audio_codec = eh_get_audio_codec;
    ctx->base.get_display = eh_get_display;
    ctx->base.destroy = eh_destroy;

    init_buttons(ctx);
    init_led(ctx);

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    esp_event_loop_create_default();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                               &wifi_event_handler, ctx));
#endif

    init_spi();
    init_lcd_display(ctx);
    init_tools(ctx);

    return &ctx->base;
}

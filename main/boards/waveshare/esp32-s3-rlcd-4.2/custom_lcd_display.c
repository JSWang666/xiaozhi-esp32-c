#include "config.h"
#include "display/display.h"
#include "c_api/display_c_api.h"

#include <stdint.h>

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

display_t *custom_rlcd_display_create(const spi_display_config_t *cfg, int width, int height);

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_lcd_io_spi.h>
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define TAG "CustomDisplay"

typedef struct {
    int mosi;
    int scl;
    int dc;
    int cs;
    int rst;
    int width;
    int height;
    esp_lcd_panel_io_handle_t io_handle;
    uint8_t *disp_buffer;
    int display_len;
    uint16_t (*pixel_index_lut)[300];
    uint8_t (*pixel_bit_lut)[300];
    lv_display_t *lv_disp;
    display_t *display;
} rlcd_ctx_t;

static rlcd_ctx_t *s_rlcd_ctx;

static void rlcd_send_command(rlcd_ctx_t *ctx, uint8_t reg)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(ctx->io_handle, reg, NULL, 0));
}

static void rlcd_send_data(rlcd_ctx_t *ctx, uint8_t data)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(ctx->io_handle, -1, &data, 1));
}

static void rlcd_send_buffer(rlcd_ctx_t *ctx, uint8_t *data, int len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(ctx->io_handle, -1, data, len));
}

static void rlcd_reset(rlcd_ctx_t *ctx)
{
    gpio_set_level((gpio_num_t)ctx->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)ctx->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)ctx->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void rlcd_color_clear(rlcd_ctx_t *ctx, uint8_t color)
{
    memset(ctx->disp_buffer, color, ctx->display_len);
}

static void init_portrait_lut(rlcd_ctx_t *ctx)
{
    uint16_t W4 = ctx->width >> 2;
    for (uint16_t y = 0; y < (uint16_t)ctx->height; y++) {
        uint16_t byte_y = y >> 1;
        uint8_t local_y = y & 1;
        for (uint16_t x = 0; x < (uint16_t)ctx->width; x++) {
            uint16_t byte_x = x >> 2;
            uint8_t local_x = x & 3;
            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);
            ctx->pixel_index_lut[x][y] = index;
            ctx->pixel_bit_lut[x][y] = (1 << bit);
        }
    }
}

static void init_landscape_lut(rlcd_ctx_t *ctx)
{
    uint16_t H4 = ctx->height >> 2;
    for (uint16_t y = 0; y < (uint16_t)ctx->height; y++) {
        uint16_t inv_y = ctx->height - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t local_y = inv_y & 3;
        for (uint16_t x = 0; x < (uint16_t)ctx->width; x++) {
            uint16_t byte_x = x >> 1;
            uint8_t local_x = x & 1;
            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);
            ctx->pixel_index_lut[x][y] = index;
            ctx->pixel_bit_lut[x][y] = (1 << bit);
        }
    }
}

static void rlcd_set_pixel(rlcd_ctx_t *ctx, uint16_t x, uint16_t y, uint8_t color)
{
    uint32_t idx = ctx->pixel_index_lut[x][y];
    uint8_t mask = ctx->pixel_bit_lut[x][y];
    uint8_t *p = &ctx->disp_buffer[idx];
    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

static void rlcd_display(rlcd_ctx_t *ctx)
{
    rlcd_send_command(ctx, 0x2A);
    rlcd_send_data(ctx, 0x12);
    rlcd_send_data(ctx, 0x2A);

    rlcd_send_command(ctx, 0x2B);
    rlcd_send_data(ctx, 0x00);
    rlcd_send_data(ctx, 0xC7);

    rlcd_send_command(ctx, 0x2c);
    rlcd_send_buffer(ctx, ctx->disp_buffer, ctx->display_len);
}

static void rlcd_init(rlcd_ctx_t *ctx)
{
    rlcd_reset(ctx);

    rlcd_send_command(ctx, 0xD6);
    rlcd_send_data(ctx, 0x17);
    rlcd_send_data(ctx, 0x02);

    rlcd_send_command(ctx, 0xD1);
    rlcd_send_data(ctx, 0x01);

    rlcd_send_command(ctx, 0xC0);
    rlcd_send_data(ctx, 0x11);
    rlcd_send_data(ctx, 0x04);

    rlcd_send_command(ctx, 0xC1);
    rlcd_send_data(ctx, 0x69);
    rlcd_send_data(ctx, 0x69);
    rlcd_send_data(ctx, 0x69);
    rlcd_send_data(ctx, 0x69);

    rlcd_send_command(ctx, 0xC2);
    rlcd_send_data(ctx, 0x19);
    rlcd_send_data(ctx, 0x19);
    rlcd_send_data(ctx, 0x19);
    rlcd_send_data(ctx, 0x19);

    rlcd_send_command(ctx, 0xC4);
    rlcd_send_data(ctx, 0x4B);
    rlcd_send_data(ctx, 0x4B);
    rlcd_send_data(ctx, 0x4B);
    rlcd_send_data(ctx, 0x4B);

    rlcd_send_command(ctx, 0xC5);
    rlcd_send_data(ctx, 0x19);
    rlcd_send_data(ctx, 0x19);
    rlcd_send_data(ctx, 0x19);
    rlcd_send_data(ctx, 0x19);

    rlcd_send_command(ctx, 0xD8);
    rlcd_send_data(ctx, 0x80);
    rlcd_send_data(ctx, 0xE9);

    rlcd_send_command(ctx, 0xB2);
    rlcd_send_data(ctx, 0x02);

    rlcd_send_command(ctx, 0xB3);
    rlcd_send_data(ctx, 0xE5);
    rlcd_send_data(ctx, 0xF6);
    rlcd_send_data(ctx, 0x05);
    rlcd_send_data(ctx, 0x46);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x76);
    rlcd_send_data(ctx, 0x45);

    rlcd_send_command(ctx, 0xB4);
    rlcd_send_data(ctx, 0x05);
    rlcd_send_data(ctx, 0x46);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x77);
    rlcd_send_data(ctx, 0x76);
    rlcd_send_data(ctx, 0x45);

    rlcd_send_command(ctx, 0x62);
    rlcd_send_data(ctx, 0x32);
    rlcd_send_data(ctx, 0x03);
    rlcd_send_data(ctx, 0x1F);

    rlcd_send_command(ctx, 0xB7);
    rlcd_send_data(ctx, 0x13);

    rlcd_send_command(ctx, 0xB0);
    rlcd_send_data(ctx, 0x64);

    rlcd_send_command(ctx, 0x11);
    vTaskDelay(pdMS_TO_TICKS(200));
    rlcd_send_command(ctx, 0xC9);
    rlcd_send_data(ctx, 0x00);

    rlcd_send_command(ctx, 0x36);
    rlcd_send_data(ctx, 0x48);

    rlcd_send_command(ctx, 0x3A);
    rlcd_send_data(ctx, 0x11);

    rlcd_send_command(ctx, 0xB9);
    rlcd_send_data(ctx, 0x20);

    rlcd_send_command(ctx, 0xB8);
    rlcd_send_data(ctx, 0x29);

    rlcd_send_command(ctx, 0x21);

    rlcd_send_command(ctx, 0x2A);
    rlcd_send_data(ctx, 0x12);
    rlcd_send_data(ctx, 0x2A);

    rlcd_send_command(ctx, 0x2B);
    rlcd_send_data(ctx, 0x00);
    rlcd_send_data(ctx, 0xC7);

    rlcd_send_command(ctx, 0x35);
    rlcd_send_data(ctx, 0x00);

    rlcd_send_command(ctx, 0xD0);
    rlcd_send_data(ctx, 0xFF);

    rlcd_send_command(ctx, 0x38);
    rlcd_send_command(ctx, 0x29);

    rlcd_color_clear(ctx, 0xff);
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p)
{
    assert(disp != NULL);
    rlcd_ctx_t *ctx = (rlcd_ctx_t *)lv_display_get_user_data(disp);
    uint16_t *buf = (uint16_t *)color_p;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buf < 0x7fff) ? 0 : 0xff;
            rlcd_set_pixel(ctx, x, y, color);
            buf++;
        }
    }
    rlcd_display(ctx);
    lv_disp_flush_ready(disp);
}

display_t *custom_rlcd_display_create(const spi_display_config_t *cfg, int width, int height)
{
    rlcd_ctx_t *ctx = calloc(1, sizeof(rlcd_ctx_t));
    if (!ctx) return NULL;

    ctx->mosi = cfg->mosi;
    ctx->scl = cfg->scl;
    ctx->dc = cfg->dc;
    ctx->cs = cfg->cs;
    ctx->rst = cfg->rst;
    ctx->width = width;
    ctx->height = height;
    s_rlcd_ctx = ctx;

    int transfer = width * height;

    ESP_LOGI(TAG, "Initialize SPI");
    spi_bus_config_t buscfg = {
        .mosi_io_num = ctx->mosi,
        .miso_io_num = -1,
        .sclk_io_num = ctx->scl,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = transfer,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = ctx->dc,
        .cs_gpio_num = ctx->cs,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 7,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &ctx->io_handle));

    gpio_config_t gpio_conf = {
        .pin_bit_mask = (0x1ULL << ctx->rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    gpio_set_level((gpio_num_t)ctx->rst, 1);

    ctx->display_len = transfer >> 3;
    ctx->disp_buffer = (uint8_t *)heap_caps_malloc(ctx->display_len, MALLOC_CAP_SPIRAM);
    assert(ctx->disp_buffer);
    ctx->pixel_index_lut = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    ctx->pixel_bit_lut = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(ctx->pixel_index_lut);
    assert(ctx->pixel_bit_lut);

    if (width == 400) {
        init_landscape_lut(ctx);
    } else {
        init_portrait_lut(ctx);
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    ctx->lv_disp = lv_display_create(width, height);
    lv_display_set_flush_cb(ctx->lv_disp, lvgl_flush_cb);
    lv_display_set_user_data(ctx->lv_disp, ctx);
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(ctx->lv_disp, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "RLCD init");
    rlcd_init(ctx);

    lvgl_port_unlock();
    if (ctx->lv_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add display");
        free(ctx);
        return no_display_create();
    }

    ctx->display = spi_lcd_display_create(ctx->io_handle, NULL, width, height, 0, 0,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    return ctx->display;
}

#include "config.h"
#include "display/display.h"
#include "c_api/display_c_api.h"

#include <stdint.h>

typedef enum {
    DRIVER_COLOR_WHITE = 0xff,
    DRIVER_COLOR_BLACK = 0x00,
} epd_color_t;

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
} custom_lcd_spi_t;

display_t *custom_epd_154_display_create(const custom_lcd_spi_t *spi_cfg, int width, int height);

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
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define TAG "CustomLcdDisplay"

#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_WIDTH * EXAMPLE_LCD_HEIGHT * BYTES_PER_PIXEL)

static const uint8_t WF_Full_1IN54[159] = {
    0x80,0x48,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x48,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x48,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x48,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xA,0x0,0x0,0x0,0x0,0x0,0x0,
    0x8,0x1,0x0,0x8,0x1,0x0,0x2,
    0xA,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x22,0x17,0x41,0x0,0x32,0x20
};

static const uint8_t WF_PARTIAL_1IN54_0[159] = {
    0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xF,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x1,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

typedef struct {
    custom_lcd_spi_t spi_data;
    int width;
    int height;
    spi_device_handle_t spi;
    uint8_t *buffer;
    lv_display_t *lv_disp;
    display_t *display;
} epd_154_ctx_t;

static epd_154_ctx_t *s_epd_ctx;

static inline void set_cs_1(epd_154_ctx_t *ctx) { gpio_set_level((gpio_num_t)ctx->spi_data.cs, 1); }
static inline void set_cs_0(epd_154_ctx_t *ctx) { gpio_set_level((gpio_num_t)ctx->spi_data.cs, 0); }
static inline void set_dc_1(epd_154_ctx_t *ctx) { gpio_set_level((gpio_num_t)ctx->spi_data.dc, 1); }
static inline void set_dc_0(epd_154_ctx_t *ctx) { gpio_set_level((gpio_num_t)ctx->spi_data.dc, 0); }
static inline void set_rst_1(epd_154_ctx_t *ctx) { gpio_set_level((gpio_num_t)ctx->spi_data.rst, 1); }
static inline void set_rst_0(epd_154_ctx_t *ctx) { gpio_set_level((gpio_num_t)ctx->spi_data.rst, 0); }

static void spi_send_byte(epd_154_ctx_t *ctx, uint8_t data)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &data };
    esp_err_t ret = spi_device_polling_transmit(ctx->spi, &t);
    assert(ret == ESP_OK);
}

static void epd_send_data(epd_154_ctx_t *ctx, uint8_t data)
{
    set_dc_1(ctx);
    set_cs_0(ctx);
    spi_send_byte(ctx, data);
    set_cs_1(ctx);
}

static void epd_send_command(epd_154_ctx_t *ctx, uint8_t command)
{
    set_dc_0(ctx);
    set_cs_0(ctx);
    spi_send_byte(ctx, command);
    set_cs_1(ctx);
}

static void epd_write_bytes(epd_154_ctx_t *ctx, const uint8_t *buf, int len)
{
    set_dc_1(ctx);
    set_cs_0(ctx);
    spi_transaction_t t = { .length = 8 * len, .tx_buffer = buf };
    esp_err_t ret = spi_device_polling_transmit(ctx->spi, &t);
    assert(ret == ESP_OK);
    set_cs_1(ctx);
}

static void read_busy(epd_154_ctx_t *ctx)
{
    while (gpio_get_level((gpio_num_t)ctx->spi_data.busy) == 1) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void epd_set_windows(epd_154_ctx_t *ctx, uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye)
{
    epd_send_command(ctx, 0x44);
    epd_send_data(ctx, (xs >> 3) & 0xFF);
    epd_send_data(ctx, (xe >> 3) & 0xFF);

    epd_send_command(ctx, 0x45);
    epd_send_data(ctx, ys & 0xFF);
    epd_send_data(ctx, (ys >> 8) & 0xFF);
    epd_send_data(ctx, ye & 0xFF);
    epd_send_data(ctx, (ye >> 8) & 0xFF);
}

static void epd_set_cursor(epd_154_ctx_t *ctx, uint16_t xs, uint16_t ys)
{
    epd_send_command(ctx, 0x4E);
    epd_send_data(ctx, xs & 0xFF);

    epd_send_command(ctx, 0x4F);
    epd_send_data(ctx, ys & 0xFF);
    epd_send_data(ctx, (ys >> 8) & 0xFF);
}

static void epd_set_lut(epd_154_ctx_t *ctx, const uint8_t *lut)
{
    epd_send_command(ctx, 0x32);
    epd_write_bytes(ctx, lut, 153);
    read_busy(ctx);

    epd_send_command(ctx, 0x3f);
    epd_send_data(ctx, lut[153]);

    epd_send_command(ctx, 0x03);
    epd_send_data(ctx, lut[154]);

    epd_send_command(ctx, 0x04);
    epd_send_data(ctx, lut[155]);
    epd_send_data(ctx, lut[156]);
    epd_send_data(ctx, lut[157]);

    epd_send_command(ctx, 0x2c);
    epd_send_data(ctx, lut[158]);
}

static void epd_turn_on_display(epd_154_ctx_t *ctx)
{
    epd_send_command(ctx, 0x22);
    epd_send_data(ctx, 0xc7);
    epd_send_command(ctx, 0x20);
    read_busy(ctx);
}

static void epd_turn_on_display_part(epd_154_ctx_t *ctx)
{
    epd_send_command(ctx, 0x22);
    epd_send_data(ctx, 0xcf);
    epd_send_command(ctx, 0x20);
    read_busy(ctx);
}

static void epd_init(epd_154_ctx_t *ctx)
{
    set_rst_1(ctx);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst_0(ctx);
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst_1(ctx);
    vTaskDelay(pdMS_TO_TICKS(50));

    read_busy(ctx);
    epd_send_command(ctx, 0x12);
    read_busy(ctx);

    epd_send_command(ctx, 0x01);
    epd_send_data(ctx, 0xC7);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x01);

    epd_send_command(ctx, 0x11);
    epd_send_data(ctx, 0x01);

    epd_set_windows(ctx, 0, ctx->width - 1, ctx->height - 1, 0);

    epd_send_command(ctx, 0x3C);
    epd_send_data(ctx, 0x01);

    epd_send_command(ctx, 0x18);
    epd_send_data(ctx, 0x80);

    epd_send_command(ctx, 0x22);
    epd_send_data(ctx, 0xB1);
    epd_send_command(ctx, 0x20);

    epd_set_cursor(ctx, 0, ctx->height - 1);
    read_busy(ctx);

    epd_set_lut(ctx, WF_Full_1IN54);
}

static void epd_clear(epd_154_ctx_t *ctx)
{
    memset(ctx->buffer, 0xff, ctx->spi_data.buffer_len);
}

static void epd_display(epd_154_ctx_t *ctx)
{
    epd_send_command(ctx, 0x24);
    assert(ctx->buffer);
    epd_write_bytes(ctx, ctx->buffer, ctx->spi_data.buffer_len);
    epd_turn_on_display(ctx);
}

static void epd_display_part_base_image(epd_154_ctx_t *ctx)
{
    epd_send_command(ctx, 0x24);
    assert(ctx->buffer);
    epd_write_bytes(ctx, ctx->buffer, ctx->spi_data.buffer_len);
    epd_send_command(ctx, 0x26);
    epd_write_bytes(ctx, ctx->buffer, ctx->spi_data.buffer_len);
    epd_turn_on_display(ctx);
}

static void epd_init_partial(epd_154_ctx_t *ctx)
{
    set_rst_1(ctx);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_rst_0(ctx);
    vTaskDelay(pdMS_TO_TICKS(20));
    set_rst_1(ctx);
    vTaskDelay(pdMS_TO_TICKS(50));

    read_busy(ctx);

    epd_set_lut(ctx, WF_PARTIAL_1IN54_0);

    epd_send_command(ctx, 0x37);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x40);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);
    epd_send_data(ctx, 0x00);

    epd_send_command(ctx, 0x3C);
    epd_send_data(ctx, 0x80);

    epd_send_command(ctx, 0x22);
    epd_send_data(ctx, 0xc0);
    epd_send_command(ctx, 0x20);
    read_busy(ctx);
}

static void epd_display_part(epd_154_ctx_t *ctx)
{
    epd_send_command(ctx, 0x24);
    assert(ctx->buffer);
    epd_write_bytes(ctx, ctx->buffer, 5000);
    epd_turn_on_display_part(ctx);
}

static void epd_draw_color_pixel(epd_154_ctx_t *ctx, uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= (uint16_t)ctx->width || y >= (uint16_t)ctx->height) {
        ESP_LOGE("EPD", "Out of bounds pixel: (%d,%d)", x, y);
        return;
    }

    uint16_t index = y * 25 + (x >> 3);
    uint8_t bit = 7 - (x & 0x07);
    if (color == DRIVER_COLOR_WHITE) {
        ctx->buffer[index] |= (0x01 << bit);
    } else {
        ctx->buffer[index] &= ~(0x01 << bit);
    }
}

static void spi_gpio_init(epd_154_ctx_t *ctx)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (0x1ULL << ctx->spi_data.rst) | (0x1ULL << ctx->spi_data.dc) | (0x1ULL << ctx->spi_data.cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    gpio_config_t busy_conf = {
        .pin_bit_mask = (0x1ULL << ctx->spi_data.busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&busy_conf));

    set_rst_1(ctx);
}

static void spi_port_init(epd_154_ctx_t *ctx)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = ctx->spi_data.mosi,
        .miso_io_num = -1,
        .sclk_io_num = ctx->spi_data.scl,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ctx->width * ctx->height,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)ctx->spi_data.spi_host, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device((spi_host_device_t)ctx->spi_data.spi_host, &devcfg, &ctx->spi));
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p)
{
    assert(disp != NULL);
    epd_154_ctx_t *ctx = (epd_154_ctx_t *)lv_display_get_user_data(disp);
    uint16_t *buf = (uint16_t *)color_p;
    epd_clear(ctx);
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buf < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            epd_draw_color_pixel(ctx, x, y, color);
            buf++;
        }
    }
    epd_display_part(ctx);
    lv_disp_flush_ready(disp);
}

display_t *custom_epd_154_display_create(const custom_lcd_spi_t *spi_cfg, int width, int height)
{
    epd_154_ctx_t *ctx = calloc(1, sizeof(epd_154_ctx_t));
    if (!ctx) return NULL;

    ctx->spi_data = *spi_cfg;
    ctx->width = width;
    ctx->height = height;
    s_epd_ctx = ctx;

    ESP_LOGI(TAG, "Initialize SPI");
    spi_port_init(ctx);
    spi_gpio_init(ctx);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    ctx->buffer = (uint8_t *)heap_caps_malloc(spi_cfg->buffer_len, MALLOC_CAP_SPIRAM);
    assert(ctx->buffer);
    ctx->lv_disp = lv_display_create(width, height);
    lv_display_set_flush_cb(ctx->lv_disp, lvgl_flush_cb);
    lv_display_set_user_data(ctx->lv_disp, ctx);

    uint8_t *buffer_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    lv_display_set_buffers(ctx->lv_disp, buffer_1, NULL, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);

    ESP_LOGI(TAG, "EPD init");
    epd_init(ctx);
    epd_clear(ctx);
    epd_display(ctx);
    epd_display_part_base_image(ctx);
    epd_init_partial(ctx);

    lvgl_port_unlock();
    if (ctx->lv_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add display");
        free(ctx);
        return no_display_create();
    }

    ctx->display = spi_lcd_display_create(NULL, NULL, width, height, 0, 0,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    return ctx->display;
}

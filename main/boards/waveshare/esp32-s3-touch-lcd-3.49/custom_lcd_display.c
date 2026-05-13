#include "config.h"
#include "display/display.h"
#include "c_api/display_c_api.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/spi_master.h>
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define TAG "CustomLcdDisplay"

static SemaphoreHandle_t s_trans_done_sem = NULL;
static uint16_t *s_trans_buf_1;

#if (DISPLAY_ROTATION_90 == true)
static uint16_t *s_dest_map;
#endif

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_lv_display;
static display_t *s_display;

static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t taskAwake = pdFALSE;
    (void)panel_io;
    (void)edata;
    assert(user_ctx != NULL);
    if (s_trans_done_sem) {
        xSemaphoreGiveFromISR(s_trans_done_sem, &taskAwake);
    }
    return false;
}

static void lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
    assert(drv != NULL);
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(drv);
    assert(panel_handle != NULL);

    lv_draw_sw_rgb565_swap(color_map, lv_area_get_width(area) * lv_area_get_height(area));

#if (DISPLAY_ROTATION_90 == true)
    lv_display_rotation_t rotation = lv_display_get_rotation(drv);
    lv_area_t rotated_area;
    if (rotation != LV_DISPLAY_ROTATION_0) {
        lv_color_format_t cf = lv_display_get_color_format(drv);
        rotated_area = *area;
        lv_display_rotate_area(drv, &rotated_area);
        uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
        uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
        int32_t src_w = lv_area_get_width(area);
        int32_t src_h = lv_area_get_height(area);
        lv_draw_sw_rotate(color_map, s_dest_map, src_w, src_h, src_stride, dest_stride, rotation, cf);
        area = &rotated_area;
    }
#endif

    const int flush_count = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (DISPLAY_HEIGHT / flush_count);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = DISPLAY_WIDTH;
    int offsety2 = offgap;

#if (DISPLAY_ROTATION_90 == true)
    uint16_t *map = (uint16_t *)s_dest_map;
#else
    uint16_t *map = (uint16_t *)color_map;
#endif

    xSemaphoreGive(s_trans_done_sem);

    for (int i = 0; i < flush_count; i++) {
        xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
        memcpy(s_trans_buf_1, map, LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, s_trans_buf_1);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

display_t *custom_lcd_349_display_create(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
{
    s_panel_io = panel_io;
    s_panel = panel;

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    s_trans_done_sem = xSemaphoreCreateBinary();
    s_trans_buf_1 = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);

    uint8_t color_bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    uint32_t buffer_size = width * height;

    lvgl_port_lock(0);
    s_lv_display = lv_display_create(width, height);
    lv_display_set_flush_cb(s_lv_display, lvgl_port_flush_callback);

    lv_color_t *buf1 = (lv_color_t *)heap_caps_aligned_alloc(1, buffer_size * color_bytes, MALLOC_CAP_SPIRAM);
#if (DISPLAY_ROTATION_90 == true)
    s_dest_map = (uint16_t *)heap_caps_malloc(buffer_size * color_bytes, MALLOC_CAP_SPIRAM);
    lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_90);
#endif
    lv_display_set_buffers(s_lv_display, buf1, NULL, buffer_size * color_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(s_lv_display, panel);
    lvgl_port_unlock();

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_io_ready_callback,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, s_lv_display);

    if (s_lv_display == NULL) {
        ESP_LOGE(TAG, "Failed to add display");
        return no_display_create();
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(s_lv_display, offset_x, offset_y);
    }

    s_display = spi_lcd_display_create(panel_io, panel, width, height, offset_x, offset_y,
        mirror_x, mirror_y, swap_xy);
    return s_display;
}

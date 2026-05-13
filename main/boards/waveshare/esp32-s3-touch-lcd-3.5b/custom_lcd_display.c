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
static uint16_t *s_trans_act;
static uint16_t *s_trans_buf_1;
static uint16_t *s_trans_buf_2;

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
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_driver_data(drv);
    assert(panel_handle != NULL);

    size_t len = lv_area_get_size(area);
    lv_draw_sw_rgb565_swap(color_map, len);

    const int x_start = area->x1;
    const int x_end = area->x2;
    const int y_start = area->y1;
    const int y_end = area->y2;
    const int width = x_end - x_start + 1;
    const int height = y_end - y_start + 1;

    int32_t hor_res = lv_display_get_horizontal_resolution(drv);
    int32_t ver_res = lv_display_get_vertical_resolution(drv);

    uint16_t *from = (uint16_t *)color_map;
    uint16_t *to = NULL;

    if (DISPLAY_TRANS_SIZE > 0) {
        assert(s_trans_buf_1 != NULL);

        int x_draw_start = 0;
        int x_draw_end = 0;
        int y_draw_start = 0;
        int y_draw_end = 0;
        int trans_count = 0;

        s_trans_act = s_trans_buf_1;
        lv_display_rotation_t rotate = LV_DISPLAY_ROTATION;

        int x_start_tmp = 0;
        int x_end_tmp = 0;
        int max_width = 0;
        int trans_width = 0;

        int y_start_tmp = 0;
        int y_end_tmp = 0;
        int max_height = 0;
        int trans_height = 0;

        if (LV_DISPLAY_ROTATION_270 == rotate || LV_DISPLAY_ROTATION_90 == rotate) {
            max_width = ((DISPLAY_TRANS_SIZE / height) > width) ? (width) : (DISPLAY_TRANS_SIZE / height);
            trans_count = width / max_width + (width % max_width ? 1 : 0);
            x_start_tmp = x_start;
            x_end_tmp = x_end;
        } else {
            max_height = ((DISPLAY_TRANS_SIZE / width) > height) ? (height) : (DISPLAY_TRANS_SIZE / width);
            trans_count = height / max_height + (height % max_height ? 1 : 0);
            y_start_tmp = y_start;
            y_end_tmp = y_end;
        }

        for (int i = 0; i < trans_count; i++) {
            if (LV_DISPLAY_ROTATION_90 == rotate) {
                trans_width = (x_end - x_start_tmp + 1) > max_width ? max_width : (x_end - x_start_tmp + 1);
                x_end_tmp = (x_end - x_start_tmp + 1) > max_width ? (x_start_tmp + max_width - 1) : x_end;
            } else if (LV_DISPLAY_ROTATION_270 == rotate) {
                trans_width = (x_end_tmp - x_start + 1) > max_width ? max_width : (x_end_tmp - x_start + 1);
                x_start_tmp = (x_end_tmp - x_start + 1) > max_width ? (x_end_tmp - trans_width + 1) : x_start;
            } else if (LV_DISPLAY_ROTATION_0 == rotate) {
                trans_height = (y_end - y_start_tmp + 1) > max_height ? max_height : (y_end - y_start_tmp + 1);
                y_end_tmp = (y_end - y_start_tmp + 1) > max_height ? (y_start_tmp + max_height - 1) : y_end;
            } else {
                trans_height = (y_end_tmp - y_start + 1) > max_height ? max_height : (y_end_tmp - y_start + 1);
                y_start_tmp = (y_end_tmp - y_start + 1) > max_height ? (y_end_tmp - max_height + 1) : y_start;
            }

            s_trans_act = (s_trans_act == s_trans_buf_1) ? s_trans_buf_2 : s_trans_buf_1;
            to = s_trans_act;

            switch (rotate) {
            case LV_DISPLAY_ROTATION_90:
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < trans_width; x++) {
                        *(to + x * height + (height - y - 1)) = *(from + y * width + x_start_tmp + x);
                    }
                }
                x_draw_start = ver_res - y_end - 1;
                x_draw_end = ver_res - y_start - 1;
                y_draw_start = x_start_tmp;
                y_draw_end = x_end_tmp;
                break;
            case LV_DISPLAY_ROTATION_270:
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < trans_width; x++) {
                        *(to + (trans_width - x - 1) * height + y) = *(from + y * width + x_start_tmp + x);
                    }
                }
                x_draw_start = y_start;
                x_draw_end = y_end;
                y_draw_start = hor_res - x_end_tmp - 1;
                y_draw_end = hor_res - x_start_tmp - 1;
                break;
            case LV_DISPLAY_ROTATION_180:
                for (int y = 0; y < trans_height; y++) {
                    for (int x = 0; x < width; x++) {
                        *(to + (trans_height - y - 1) * width + (width - x - 1)) = *(from + y_start_tmp * width + y * width + x);
                    }
                }
                x_draw_start = hor_res - x_end - 1;
                x_draw_end = hor_res - x_start - 1;
                y_draw_start = ver_res - y_end_tmp - 1;
                y_draw_end = ver_res - y_start_tmp - 1;
                break;
            case LV_DISPLAY_ROTATION_0:
                for (int y = 0; y < trans_height; y++) {
                    for (int x = 0; x < width; x++) {
                        *(to + y * width + x) = *(from + y_start_tmp * width + y * width + x);
                    }
                }
                x_draw_start = x_start;
                x_draw_end = x_end;
                y_draw_start = y_start_tmp;
                y_draw_end = y_end_tmp;
                break;
            default:
                break;
            }

            if (0 == i) {
                xSemaphoreGive(s_trans_done_sem);
            }

            xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
            esp_lcd_panel_draw_bitmap(panel_handle, x_draw_start, y_draw_start, x_draw_end + 1, y_draw_end + 1, to);

            if (LV_DISPLAY_ROTATION_90 == rotate) {
                x_start_tmp += max_width;
            } else if (LV_DISPLAY_ROTATION_270 == rotate) {
                x_end_tmp -= max_width;
            }
            if (LV_DISPLAY_ROTATION_0 == rotate) {
                y_start_tmp += max_height;
            } else {
                y_end_tmp -= max_height;
            }
        }
    } else {
        esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end + 1, y_end + 1, color_map);
    }
    lv_disp_flush_ready(drv);
}

display_t *custom_lcd_35b_display_create(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
{
    s_panel_io = panel_io;
    s_panel = panel;

    {
        uint16_t white_line[DISPLAY_WIDTH];
        memset(white_line, 0xFF, sizeof(white_line));
        for (int y = 0; y < height; y++) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, width, y + 1, white_line);
        }
    }

    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    s_trans_done_sem = xSemaphoreCreateCounting(1, 0);
    s_trans_buf_1 = (uint16_t *)heap_caps_malloc(DISPLAY_TRANS_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    s_trans_buf_2 = (uint16_t *)heap_caps_malloc(DISPLAY_TRANS_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);

    uint8_t color_bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    uint32_t buffer_size = width * height;

    lvgl_port_lock(0);
    s_lv_display = lv_display_create(width, height);
    lv_display_set_flush_cb(s_lv_display, lvgl_port_flush_callback);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_aligned_alloc(1, buffer_size * color_bytes, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(s_lv_display, buf1, NULL, buffer_size * color_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_driver_data(s_lv_display, panel);
    lvgl_port_unlock();

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_io_ready_callback,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, s_lv_display);

    esp_lcd_panel_disp_on_off(panel, false);

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

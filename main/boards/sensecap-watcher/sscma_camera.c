#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>
#include <esp_io_expander_tca95xx_16bit.h>
#include <esp_jpeg_dec.h>
#include <sscma_client.h>

#include "c_api/app_c_api.h"
#include "c_api/mcp_server_c_api.h"
#include "device_state.h"
#include "sscma_client_commands.h"
#include "boards/sensecap-watcher/sscma_camera_api.h"

#define TAG "SscmaCamera"

#define IMG_JPEG_BUF_SIZE   (48 * 1024)

typedef struct {
    uint8_t *img;
    size_t len;
} sscma_data_t;

typedef struct {
    uint8_t *buf;
    size_t len;
} jpeg_data_t;

typedef enum {
    DETECT_IDLE = 0,
    DETECT_VALIDATING,
    DETECT_COOLDOWN,
} detection_state_t;

struct sscma_camera {
    sscma_client_io_handle_t io_handle;
    sscma_client_handle_t client_handle;
    QueueHandle_t data_queue;

    jpeg_data_t jpeg_data;
    jpeg_dec_handle_t jpeg_dec;
    jpeg_dec_io_t *jpeg_io;
    jpeg_dec_header_info_t *jpeg_out;

    detection_state_t detection_state;
    int64_t state_start_time;
    bool need_start_cooldown;
    int64_t last_detected_time;

    int detect_target;
    int detect_threshold;
    int detect_duration_sec;
    int detect_invoke_interval_sec;
    int detect_debounce_sec;
    int inference_en;
    bool sscma_restarted;

    sscma_client_model_t *model;
    int model_class_cnt;

    esp_io_expander_handle_t io_exp_handle;
};

static bool himax_keepalive_check(sscma_client_handle_t client)
{
    sscma_client_reply_t reply = {0};
    int retry = 3;
    while (retry--) {
        esp_err_t ret = sscma_client_request(client, CMD_PREFIX CMD_AT_ID CMD_QUERY CMD_SUFFIX, &reply, true, pdMS_TO_TICKS(2000));
        if (reply.payload) sscma_client_reply_clear(&reply);
        if (ret == ESP_OK) return true;
        ESP_LOGE(TAG, "Himax keepalive check failed: %d", ret);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static void on_event_cb(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    sscma_camera_t *self = (sscma_camera_t *)user_ctx;
    if (!self) return;

    int width = 0, height = 0;
    cJSON *data = cJSON_GetObjectItem(reply->payload, "data");
    if (data && cJSON_IsObject(data)) {
        cJSON *res = cJSON_GetObjectItem(data, "resolution");
        if (res && cJSON_IsArray(res) && cJSON_GetArraySize(res) == 2) {
            width = cJSON_GetArrayItem(res, 0)->valueint;
            height = cJSON_GetArrayItem(res, 1)->valueint;
        }
    }

    switch (width + height) {
    case (416 + 416): {
        bool is_object_detected = false;
        bool is_need_wake = false;
        int64_t cur_tm = esp_timer_get_time();
        int obj_cnt = 0;
        int model_type = 0;
        int box_count = 0;
        sscma_client_box_t *boxes = NULL;
        int class_count = 0;
        sscma_client_class_t *classes = NULL;
        int point_count = 0;
        sscma_client_point_t *points = NULL;

        if (sscma_utils_fetch_boxes_from_reply(reply, &boxes, &box_count) == ESP_OK && box_count > 0) {
            for (int i = 0; i < box_count; i++) {
                if (boxes[i].target == self->detect_target && boxes[i].score > self->detect_threshold) {
                    is_object_detected = true;
                    model_type = 0;
                    obj_cnt++;
                    break;
                }
            }
            free(boxes);
        } else if (sscma_utils_fetch_classes_from_reply(reply, &classes, &class_count) == ESP_OK && class_count > 0) {
            for (int i = 0; i < class_count; i++) {
                if (classes[i].target == self->detect_target && classes[i].score > self->detect_threshold) {
                    is_object_detected = true;
                    model_type = 1;
                    obj_cnt++;
                }
            }
            free(classes);
        } else if (sscma_utils_fetch_points_from_reply(reply, &points, &point_count) == ESP_OK && point_count > 0) {
            for (int i = 0; i < point_count; i++) {
                if (points[i].target == self->detect_target && points[i].score > self->detect_threshold) {
                    is_object_detected = true;
                    model_type = 2;
                    obj_cnt++;
                }
            }
            free(points);
        }

        if (self->need_start_cooldown) {
            self->state_start_time = cur_tm;
            self->need_start_cooldown = false;
        }

        switch (self->detection_state) {
        case DETECT_IDLE:
            if (is_object_detected) {
                self->detection_state = DETECT_VALIDATING;
                self->state_start_time = cur_tm;
                self->last_detected_time = cur_tm;
            }
            break;
        case DETECT_VALIDATING:
            if (is_object_detected) {
                self->last_detected_time = cur_tm;
                if ((cur_tm - self->state_start_time) >= (self->detect_duration_sec * 1000000LL))
                    is_need_wake = true;
            } else {
                if (self->last_detected_time > 0 &&
                    (cur_tm - self->last_detected_time) >= self->detect_debounce_sec * 1000000LL) {
                    self->detection_state = DETECT_IDLE;
                    self->last_detected_time = 0;
                }
            }
            break;
        case DETECT_COOLDOWN:
            if (!is_object_detected &&
                (cur_tm - self->state_start_time) >= (self->detect_invoke_interval_sec * 1000000LL)) {
                self->detection_state = DETECT_IDLE;
            }
            break;
        }

        if (is_need_wake) {
            const char *target_name = "object";
            if (self->model && self->model->classes[self->detect_target])
                target_name = self->model->classes[self->detect_target];

            char wake_word[128];
            snprintf(wake_word, sizeof(wake_word), "<detect>%d %s detected </detect>", obj_cnt, target_name);

            app_context_t *app = app_get_context();
            if (app) app_wake_word_invoke(app, wake_word);

            self->detection_state = DETECT_COOLDOWN;
            self->need_start_cooldown = true;
        }
        break;
    }
    case (640 + 480): {
        char *img = NULL;
        int img_size = 0;
        if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) == ESP_OK) {
            sscma_data_t sdata = { .img = (uint8_t *)img, .len = img_size };
            sscma_data_t dummy;
            while (xQueueReceive(self->data_queue, &dummy, 0) == pdPASS) {
                if (dummy.img) heap_caps_free(dummy.img);
            }
            xQueueSend(self->data_queue, &sdata, 0);
        }
        break;
    }
    default:
        break;
    }
}

static void on_connect_cb(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "SSCMA client connected");
    sscma_camera_t *self = (sscma_camera_t *)user_ctx;
    if (self) self->sscma_restarted = true;
}

static void on_log_cb(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "log: %s", reply->data);
}

static void sscma_monitor_task(void *arg)
{
    sscma_camera_t *self = (sscma_camera_t *)arg;
    bool is_inference = false;
    int64_t last_keepalive = esp_timer_get_time();

    while (true) {
        if (self->sscma_restarted) {
            self->sscma_restarted = false;
            is_inference = false;
        }

        if (esp_timer_get_time() - last_keepalive > 10 * 1000000) {
            last_keepalive = esp_timer_get_time();
            if (!himax_keepalive_check(self->client_handle)) {
                ESP_LOGE(TAG, "restart himax");
                sscma_client_reset(self->client_handle);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        app_context_t *app = app_get_context();
        int dev_state = app ? app_get_device_state(app) : -1;

        if (self->inference_en && dev_state == kDeviceStateIdle) {
            if (!is_inference) {
                sscma_client_break(self->client_handle);
                sscma_client_set_model(self->client_handle, 4);
                sscma_client_set_sensor(self->client_handle, 1, 1, true);
                sscma_client_invoke(self->client_handle, -1, false, true);
                is_inference = true;
            }
        } else if (is_inference && (!self->inference_en || dev_state != kDeviceStateIdle)) {
            is_inference = false;
            sscma_client_break(self->client_handle);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

sscma_camera_t *sscma_camera_create(esp_io_expander_handle_t io_exp_handle)
{
    sscma_camera_t *self = calloc(1, sizeof(sscma_camera_t));
    if (!self) return NULL;

    self->io_exp_handle = io_exp_handle;
    self->detect_threshold = 75;
    self->detect_invoke_interval_sec = 8;
    self->detect_duration_sec = 2;
    self->detect_debounce_sec = 1;

    sscma_client_io_spi_config_t spi_io_cfg = {0};
    spi_io_cfg.sync_gpio_num = BSP_SSCMA_CLIENT_SPI_SYNC;
    spi_io_cfg.cs_gpio_num = BSP_SSCMA_CLIENT_SPI_CS;
    spi_io_cfg.pclk_hz = BSP_SSCMA_CLIENT_SPI_CLK;
    spi_io_cfg.spi_mode = 0;
    spi_io_cfg.wait_delay = 10;
    spi_io_cfg.io_expander = io_exp_handle;
    spi_io_cfg.flags.sync_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new_io_spi_bus((sscma_client_spi_bus_handle_t)BSP_SSCMA_CLIENT_SPI_NUM, &spi_io_cfg, &self->io_handle);

    sscma_client_config_t sscma_cfg = SSCMA_CLIENT_CONFIG_DEFAULT();
    sscma_cfg.event_queue_size = CONFIG_SSCMA_EVENT_QUEUE_SIZE;
    sscma_cfg.tx_buffer_size = CONFIG_SSCMA_TX_BUFFER_SIZE;
    sscma_cfg.rx_buffer_size = CONFIG_SSCMA_RX_BUFFER_SIZE;
    sscma_cfg.process_task_stack = CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE;
    sscma_cfg.process_task_affinity = CONFIG_SSCMA_PROCESS_TASK_AFFINITY;
    sscma_cfg.process_task_priority = CONFIG_SSCMA_PROCESS_TASK_PRIORITY;
    sscma_cfg.monitor_task_stack = CONFIG_SSCMA_MONITOR_TASK_STACK_SIZE;
    sscma_cfg.monitor_task_affinity = CONFIG_SSCMA_MONITOR_TASK_AFFINITY;
    sscma_cfg.monitor_task_priority = CONFIG_SSCMA_MONITOR_TASK_PRIORITY;
    sscma_cfg.reset_gpio_num = BSP_SSCMA_CLIENT_RST;
    sscma_cfg.io_expander = io_exp_handle;
    sscma_cfg.flags.reset_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new(self->io_handle, &sscma_cfg, &self->client_handle);
    self->data_queue = xQueueCreate(1, sizeof(sscma_data_t));

    sscma_client_callback_t cb = {0};
    cb.on_event = on_event_cb;
    cb.on_connect = on_connect_cb;
    cb.on_log = on_log_cb;
    sscma_client_register_callback(self->client_handle, &cb, self);
    sscma_client_init(self->client_handle);

    if (sscma_client_set_sensor(self->client_handle, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        sscma_client_del(self->client_handle);
        self->client_handle = NULL;
        return self;
    }

    sscma_client_info_t *info;
    if (sscma_client_get_info(self->client_handle, &info, true) == ESP_OK) {
        ESP_LOGI(TAG, "Device Info - ID: %s, Name: %s",
            info->id ? info->id : "NULL", info->name ? info->name : "NULL");
    }

    self->jpeg_data.buf = heap_caps_malloc(IMG_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!self->jpeg_data.buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG buffer");
        return self;
    }

    jpeg_dec_config_t dec_cfg = { .output_type = JPEG_PIXEL_FORMAT_RGB565_LE, .rotate = JPEG_ROTATE_0D };
    if (jpeg_dec_open(&dec_cfg, &self->jpeg_dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        return self;
    }

    self->jpeg_io = heap_caps_calloc(1, sizeof(jpeg_dec_io_t), MALLOC_CAP_SPIRAM);
    self->jpeg_out = heap_caps_aligned_alloc(16, sizeof(jpeg_dec_header_info_t), MALLOC_CAP_SPIRAM);
    if (self->jpeg_out) memset(self->jpeg_out, 0, sizeof(jpeg_dec_header_info_t));

    sscma_client_set_model(self->client_handle, 4);
    self->model_class_cnt = 0;
    if (sscma_client_get_model(self->client_handle, &self->model, true) == ESP_OK) {
        if (self->model && self->model->classes[0]) {
            for (int i = 0; self->model->classes[i]; i++)
                self->model_class_cnt++;
        }
    }

    xTaskCreate(sscma_monitor_task, "sscma_camera", 4096, self, 1, NULL);

    return self;
}

void sscma_camera_destroy(sscma_camera_t *self)
{
    if (!self) return;
    if (self->jpeg_data.buf) heap_caps_free(self->jpeg_data.buf);
    if (self->jpeg_dec) jpeg_dec_close(self->jpeg_dec);
    if (self->jpeg_io) heap_caps_free(self->jpeg_io);
    if (self->jpeg_out) heap_caps_free(self->jpeg_out);
    if (self->client_handle) sscma_client_del(self->client_handle);
    if (self->data_queue) vQueueDelete(self->data_queue);
    free(self);
}

esp_err_t sscma_camera_capture_still(sscma_camera_t *self, int timeout_ms)
{
    if (!self || !self->client_handle || !self->data_queue || !self->jpeg_data.buf) {
        return ESP_ERR_INVALID_STATE;
    }

    sscma_data_t dummy;
    while (xQueueReceive(self->data_queue, &dummy, 0) == pdPASS) {
        if (dummy.img) {
            heap_caps_free(dummy.img);
        }
    }

    self->jpeg_data.len = 0;

    (void)sscma_client_break(self->client_handle);
    vTaskDelay(pdMS_TO_TICKS(80));

    if (sscma_client_set_model(self->client_handle, 4) != ESP_OK) {
        ESP_LOGW(TAG, "capture_still: set_model failed");
    }
    if (sscma_client_set_sensor(self->client_handle, 1, 3, true) != ESP_OK) {
        ESP_LOGW(TAG, "capture_still: set_sensor failed");
    }

    if (sscma_client_invoke(self->client_handle, 1, false, true) != ESP_OK) {
        ESP_LOGW(TAG, "capture_still: invoke failed");
    }

    sscma_data_t sdata = {0};
    TickType_t wait = (timeout_ms <= 0) ? pdMS_TO_TICKS(8000) : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(self->data_queue, &sdata, wait) != pdPASS) {
        (void)sscma_client_break(self->client_handle);
        return ESP_ERR_TIMEOUT;
    }

    if (!sdata.img || sdata.len == 0) {
        if (sdata.img) {
            heap_caps_free(sdata.img);
        }
        (void)sscma_client_break(self->client_handle);
        return ESP_FAIL;
    }

    size_t copy_len = sdata.len < IMG_JPEG_BUF_SIZE ? sdata.len : IMG_JPEG_BUF_SIZE;
    memcpy(self->jpeg_data.buf, sdata.img, copy_len);
    self->jpeg_data.len = copy_len;
    heap_caps_free(sdata.img);

    (void)sscma_client_break(self->client_handle);
    return ESP_OK;
}

const uint8_t *sscma_camera_last_jpeg(const sscma_camera_t *self, size_t *out_len)
{
    if (!self || !out_len) {
        return NULL;
    }
    *out_len = self->jpeg_data.len;
    if (self->jpeg_data.len == 0 || !self->jpeg_data.buf) {
        return NULL;
    }
    return self->jpeg_data.buf;
}

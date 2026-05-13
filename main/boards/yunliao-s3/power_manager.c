#include "power_manager_c.h"
#include "config.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <string.h>

#define TAG "PowerManager"

static QueueHandle_t gpio_evt_queue = NULL;
static uint16_t battCnt;
static int battLife = 70;

static void IRAM_ATTR batt_mon_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void batt_mon_task(void *arg)
{
    uint32_t io_num;
    while (1) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            battCnt++;
        }
    }
}

static void calBattLife(void)
{
    battLife = battCnt;
    if (battLife > 100) battLife = 100;
    battCnt = 0;
}

struct yunliao_pm {
    esp_timer_handle_t timer_handle;
    yunliao_pm_cb_t charging_callback;
    void *charging_cb_ud;
    yunliao_pm_cb_t discharging_callback;
    void *discharging_cb_ud;
    yunliao_pm_cb_t bt_link_callback;
    void *bt_link_cb_ud;
    int is_charging;
    int is_discharging;
    int call_count;
    TaskHandle_t bt_task_handle;
};

static void check_battery_status(yunliao_pm_t *pm)
{
    pm->call_count++;
    if (pm->call_count >= MON_BATT_CNT) {
        calBattLife();
        pm->call_count = 0;
    }

    bool new_charging = yunliao_pm_is_charging(pm);
    if ((int)new_charging != pm->is_charging) {
        pm->is_charging = new_charging;
        if (pm->charging_callback)
            pm->charging_callback(new_charging, pm->charging_cb_ud);
    }

    bool new_discharging = yunliao_pm_is_discharging(pm);
    if ((int)new_discharging != pm->is_discharging) {
        pm->is_discharging = new_discharging;
        if (pm->discharging_callback)
            pm->discharging_callback(new_discharging, pm->discharging_cb_ud);
    }
}

static void battery_timer_cb(void *arg)
{
    yunliao_pm_t *pm = (yunliao_pm_t *)arg;
    check_battery_status(pm);
}

static void bt_task(void *arg)
{
    yunliao_pm_t *pm = (yunliao_pm_t *)arg;
    int last_level = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int level = gpio_get_level(MON_BTLINK_PIN);
        if (level != last_level) {
            last_level = level;
            if (pm->bt_link_callback)
                pm->bt_link_callback(level == 1, pm->bt_link_cb_ud);
        }
    }
}

yunliao_pm_t *yunliao_pm_create(void)
{
    yunliao_pm_t *pm = calloc(1, sizeof(yunliao_pm_t));
    if (!pm) return NULL;
    pm->is_charging = -1;
    pm->is_discharging = -1;
    return pm;
}

void yunliao_pm_initialize(yunliao_pm_t *pm)
{
    gpio_config_t io_conf_5v = {
        .pin_bit_mask = 1ULL << BOOT_5V_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf_5v));

    gpio_config_t io_conf_4g = {
        .pin_bit_mask = (1ULL << BOOT_4G5V_PIN) | (1ULL << BOOT_4GEN_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf_4g));

    gpio_config_t io_conf_batt_mon = {
        .pin_bit_mask = 1ULL << MON_BATT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf_batt_mon));

    gpio_evt_queue = xQueueCreate(2, sizeof(uint32_t));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(MON_BATT_PIN, batt_mon_isr_handler, (void *)(uint32_t)MON_BATT_PIN));
    xTaskCreate(batt_mon_task, "batt_mon_task", 1024, NULL, 10, NULL);

    gpio_config_t mon_conf = {
        .pin_bit_mask = 1ULL << MON_USB_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&mon_conf);

    esp_timer_create_args_t timer_args = {
        .callback = battery_timer_cb,
        .arg = pm,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_check_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &pm->timer_handle));
    ESP_ERROR_CHECK(esp_timer_start_periodic(pm->timer_handle, 1000000));
}

bool yunliao_pm_is_charging(yunliao_pm_t *pm)
{
    (void)pm;
    return gpio_get_level(MON_USB_PIN) == 1 && battLife < 95;
}

bool yunliao_pm_is_discharging(yunliao_pm_t *pm)
{
    (void)pm;
    return gpio_get_level(MON_USB_PIN) == 0;
}

int yunliao_pm_get_battery_level(yunliao_pm_t *pm)
{
    (void)pm;
    return battLife;
}

void yunliao_pm_on_charging_changed(yunliao_pm_t *pm, yunliao_pm_cb_t cb, void *ud)
{
    pm->charging_callback = cb;
    pm->charging_cb_ud = ud;
}

void yunliao_pm_on_discharging_changed(yunliao_pm_t *pm, yunliao_pm_cb_t cb, void *ud)
{
    pm->discharging_callback = cb;
    pm->discharging_cb_ud = ud;
}

void yunliao_pm_on_bt_link_changed(yunliao_pm_t *pm, yunliao_pm_cb_t cb, void *ud)
{
    pm->bt_link_callback = cb;
    pm->bt_link_cb_ud = ud;
}

void yunliao_pm_start_5v(yunliao_pm_t *pm)
{
    (void)pm;
    gpio_set_level(BOOT_5V_PIN, 1);
}

void yunliao_pm_shutdown_5v(yunliao_pm_t *pm)
{
    (void)pm;
    gpio_set_level(BOOT_5V_PIN, 0);
}

void yunliao_pm_start_4g(yunliao_pm_t *pm)
{
    (void)pm;
    gpio_set_level(BOOT_4G5V_PIN, 1);
}

void yunliao_pm_shutdown_4g(yunliao_pm_t *pm)
{
    (void)pm;
    gpio_set_level(BOOT_4G5V_PIN, 0);
    gpio_set_level(ML307_RX_PIN, 1);
    gpio_set_level(ML307_TX_PIN, 1);
}

void yunliao_pm_enable_4g(yunliao_pm_t *pm)
{
    (void)pm;
    gpio_set_level(BOOT_4GEN_PIN, 1);
}

void yunliao_pm_disable_4g(yunliao_pm_t *pm)
{
    (void)pm;
    gpio_set_level(BOOT_4GEN_PIN, 0);
}

void yunliao_pm_check_startup(yunliao_pm_t *pm)
{
    (void)pm;
    /* Simplified: in original code it checks NVS sleep_flag */
}

void yunliao_pm_sleep(yunliao_pm_t *pm)
{
    ESP_LOGI(TAG, "Entering deep sleep");
    yunliao_pm_disable_4g(pm);
    yunliao_pm_shutdown_4g(pm);
    yunliao_pm_shutdown_5v(pm);

    if (gpio_evt_queue) {
        vQueueDelete(gpio_evt_queue);
        gpio_evt_queue = NULL;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_remove(BOOT_BUTTON_PIN));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_PIN, 0));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_PIN));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_PIN));
    esp_deep_sleep_start();
}

void yunliao_pm_init_bt_modul(yunliao_pm_t *pm)
{
    if (MON_BTLINK_PIN == GPIO_NUM_NC) return;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << MON_BTLINK_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_conf) != ESP_OK) return;

    xTaskCreate(bt_task, "bt_task", 2048, pm, 10, &pm->bt_task_handle);
}

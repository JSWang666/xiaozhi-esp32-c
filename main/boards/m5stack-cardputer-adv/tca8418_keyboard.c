#include "i2c_device.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <esp_log.h>
#include <string.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define KEY_MOD_NONE   0x00
#define KEY_MOD_SHIFT  0x01
#define KEY_MOD_CTRL   0x02
#define KEY_MOD_ALT    0x04
#define KEY_MOD_OPT    0x08

enum {
    KC_NONE = 0x00,
    KC_A = 0x04, KC_B = 0x05, KC_C = 0x06, KC_D = 0x07,
    KC_E = 0x08, KC_F = 0x09, KC_G = 0x0A, KC_H = 0x0B,
    KC_I = 0x0C, KC_J = 0x0D, KC_K = 0x0E, KC_L = 0x0F,
    KC_M = 0x10, KC_N = 0x11, KC_O = 0x12, KC_P = 0x13,
    KC_Q = 0x14, KC_R = 0x15, KC_S = 0x16, KC_T = 0x17,
    KC_U = 0x18, KC_V = 0x19, KC_W = 0x1A, KC_X = 0x1B,
    KC_Y = 0x1C, KC_Z = 0x1D,
    KC_1 = 0x1E, KC_2 = 0x1F, KC_3 = 0x20, KC_4 = 0x21,
    KC_5 = 0x22, KC_6 = 0x23, KC_7 = 0x24, KC_8 = 0x25,
    KC_9 = 0x26, KC_0 = 0x27,
    KC_ENTER = 0x28, KC_ESC = 0x29, KC_BACKSPACE = 0x2A,
    KC_TAB = 0x2B, KC_SPACE = 0x2C, KC_MINUS = 0x2D,
    KC_EQUAL = 0x2E, KC_LBRACKET = 0x2F, KC_RBRACKET = 0x30,
    KC_BACKSLASH = 0x31, KC_SEMICOLON = 0x33, KC_APOSTROPHE = 0x34,
    KC_GRAVE = 0x35, KC_COMMA = 0x36, KC_DOT = 0x37,
    KC_SLASH = 0x38, KC_CAPSLOCK = 0x39,
    KC_RIGHT = 0x4F, KC_LEFT = 0x50, KC_DOWN = 0x51, KC_UP = 0x52,
    KC_LSHIFT = 0xE1, KC_LCTRL = 0xE0, KC_LALT = 0xE2, KC_LOPT = 0xE3,
};

typedef enum {
    TCA8418_KEY_NONE = 0,
    TCA8418_KEY_UP, TCA8418_KEY_DOWN,
    TCA8418_KEY_LEFT, TCA8418_KEY_RIGHT,
    TCA8418_KEY_ENTER, TCA8418_KEY_OTHER
} tca8418_legacy_key_t;

typedef struct {
    bool pressed;
    bool is_modifier;
    uint8_t key_code;
    const char *key_char;
} tca8418_key_event_t;

typedef struct tca8418_keyboard tca8418_keyboard_t;
typedef void (*tca8418_key_cb_t)(tca8418_legacy_key_t key, void *ud);
typedef void (*tca8418_key_event_cb_t)(const tca8418_key_event_t *event, void *ud);

tca8418_keyboard_t *tca8418_keyboard_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr, gpio_num_t int_pin);
void tca8418_keyboard_destroy(tca8418_keyboard_t *kb);
void tca8418_keyboard_initialize(tca8418_keyboard_t *kb);
void tca8418_keyboard_set_key_cb(tca8418_keyboard_t *kb, tca8418_key_cb_t cb, void *ud);
void tca8418_keyboard_set_key_event_cb(tca8418_keyboard_t *kb, tca8418_key_event_cb_t cb, void *ud);

#define TAG "TCA8418"

#define TCA8418_REG_GPIO_INT_EN_1   0x1A
#define TCA8418_REG_GPIO_INT_EN_2   0x1B
#define TCA8418_REG_GPIO_INT_EN_3   0x1C
#define TCA8418_REG_GPIO_DAT_STAT_1 0x14
#define TCA8418_REG_GPIO_DAT_STAT_2 0x15
#define TCA8418_REG_GPIO_DAT_STAT_3 0x16
#define TCA8418_REG_GPIO_DAT_OUT_1  0x17
#define TCA8418_REG_GPIO_DAT_OUT_2  0x18
#define TCA8418_REG_GPIO_DAT_OUT_3  0x19
#define TCA8418_REG_GPIO_INT_LVL_1  0x20
#define TCA8418_REG_GPIO_INT_LVL_2  0x21
#define TCA8418_REG_GPIO_INT_LVL_3  0x22
#define TCA8418_REG_DEBOUNCE_DIS_1  0x29
#define TCA8418_REG_DEBOUNCE_DIS_2  0x2A
#define TCA8418_REG_DEBOUNCE_DIS_3  0x2B
#define TCA8418_REG_GPIO_PULL_1     0x2C
#define TCA8418_REG_GPIO_PULL_2     0x2D
#define TCA8418_REG_GPIO_PULL_3     0x2E

#define TCA8418_CFG_AI              0x80
#define TCA8418_CFG_GPI_E_CFG       0x40
#define TCA8418_CFG_OVR_FLOW_M      0x20
#define TCA8418_CFG_INT_CFG         0x10
#define TCA8418_CFG_OVR_FLOW_IEN    0x08
#define TCA8418_CFG_K_LCK_IEN       0x04
#define TCA8418_CFG_GPI_IEN         0x02

#define TCA8418_INT_STAT_CAD_INT    0x10
#define TCA8418_INT_STAT_OVR_FLOW   0x08
#define TCA8418_INT_STAT_K_LCK_INT  0x04
#define TCA8418_INT_STAT_GPI_INT    0x02
#define TCA8418_INT_STAT_K_INT      0x01

typedef struct {
    const char *normal;
    uint8_t normal_code;
    const char *shifted;
    uint8_t shifted_code;
} key_value_t;

static const key_value_t KEY_MAP[4][14] = {
    /* Row 0 */
    {{"`", KC_GRAVE, "~", KC_GRAVE},
     {"1", KC_1, "!", KC_1},
     {"2", KC_2, "@", KC_2},
     {"3", KC_3, "#", KC_3},
     {"4", KC_4, "$", KC_4},
     {"5", KC_5, "%", KC_5},
     {"6", KC_6, "^", KC_6},
     {"7", KC_7, "&", KC_7},
     {"8", KC_8, "*", KC_8},
     {"9", KC_9, "(", KC_9},
     {"0", KC_0, ")", KC_0},
     {"-", KC_MINUS, "_", KC_MINUS},
     {"=", KC_EQUAL, "+", KC_EQUAL},
     {"", KC_BACKSPACE, "", KC_BACKSPACE}},
    /* Row 1 */
    {{"", KC_TAB, "", KC_TAB},
     {"q", KC_Q, "Q", KC_Q},
     {"w", KC_W, "W", KC_W},
     {"e", KC_E, "E", KC_E},
     {"r", KC_R, "R", KC_R},
     {"t", KC_T, "T", KC_T},
     {"y", KC_Y, "Y", KC_Y},
     {"u", KC_U, "U", KC_U},
     {"i", KC_I, "I", KC_I},
     {"o", KC_O, "O", KC_O},
     {"p", KC_P, "P", KC_P},
     {"[", KC_LBRACKET, "{", KC_LBRACKET},
     {"]", KC_RBRACKET, "}", KC_RBRACKET},
     {"\\", KC_BACKSLASH, "|", KC_BACKSLASH}},
    /* Row 2 */
    {{"", KC_LSHIFT, "", KC_LSHIFT},
     {"", KC_CAPSLOCK, "", KC_CAPSLOCK},
     {"a", KC_A, "A", KC_A},
     {"s", KC_S, "S", KC_S},
     {"d", KC_D, "D", KC_D},
     {"f", KC_F, "F", KC_F},
     {"g", KC_G, "G", KC_G},
     {"h", KC_H, "H", KC_H},
     {"j", KC_J, "J", KC_J},
     {"k", KC_K, "K", KC_K},
     {"l", KC_L, "L", KC_L},
     {";", KC_SEMICOLON, ":", KC_SEMICOLON},
     {"'", KC_APOSTROPHE, "\"", KC_APOSTROPHE},
     {"", KC_ENTER, "", KC_ENTER}},
    /* Row 3 */
    {{"", KC_LCTRL, "", KC_LCTRL},
     {"", KC_LOPT, "", KC_LOPT},
     {"", KC_LALT, "", KC_LALT},
     {"z", KC_Z, "Z", KC_Z},
     {"x", KC_X, "X", KC_X},
     {"c", KC_C, "C", KC_C},
     {"v", KC_V, "V", KC_V},
     {"b", KC_B, "B", KC_B},
     {"n", KC_N, "N", KC_N},
     {"m", KC_M, "M", KC_M},
     {",", KC_COMMA, "<", KC_COMMA},
     {".", KC_DOT, ">", KC_DOT},
     {"/", KC_SLASH, "?", KC_SLASH},
     {" ", KC_SPACE, " ", KC_SPACE}}
};

struct tca8418_keyboard {
    i2c_device_t *i2c_dev;
    gpio_num_t int_pin;
    tca8418_key_cb_t key_callback;
    void *key_cb_ud;
    tca8418_key_event_cb_t key_event_callback;
    void *key_event_cb_ud;
    TaskHandle_t task_handle;
    volatile bool isr_flag;
    uint8_t modifier_mask;
    bool caps_lock_on;
    uint64_t key_state_mask;
};

static bool remap_raw_key_to_logical(uint8_t *row, uint8_t *col)
{
    if (*row >= 7 || *col >= 8) return false;
    uint8_t mapped_col = (*row * 2) + ((*col > 3) ? 1 : 0);
    uint8_t mapped_row = (*col + 4) % 4;
    *row = mapped_row;
    *col = mapped_col;
    return true;
}

static uint64_t logical_key_mask(uint8_t row, uint8_t col)
{
    uint8_t idx = (row * 14) + col;
    if (idx >= 64) return 0;
    return 1ULL << idx;
}

static void configure_matrix(tca8418_keyboard_t *kb)
{
    i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_KP_GPIO_1, 0x7F);
    i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_KP_GPIO_2, 0xFF);
    i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_KP_GPIO_3, 0x00);
}

static void enable_interrupts(tca8418_keyboard_t *kb)
{
    uint8_t cfg = TCA8418_CFG_KE_IEN | TCA8418_CFG_OVR_FLOW_M | TCA8418_CFG_INT_CFG;
    i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_CFG, cfg);
}

static uint8_t get_event(tca8418_keyboard_t *kb)
{
    return i2c_device_read_reg(kb->i2c_dev, TCA8418_REG_KEY_EVENT_A);
}

static void flush_events(tca8418_keyboard_t *kb)
{
    uint8_t event;
    int count = 0;
    while ((event = get_event(kb)) != 0 && count < 10) {
        count++;
    }
    i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_INT_STAT, 0x1F);
}

static void update_modifier_state(tca8418_keyboard_t *kb, uint8_t row, uint8_t col, bool pressed)
{
    if (row == 2 && col == 0) {
        if (pressed) kb->modifier_mask |= KEY_MOD_SHIFT;
        else kb->modifier_mask &= ~KEY_MOD_SHIFT;
    } else if (row == 3 && col == 0) {
        if (pressed) kb->modifier_mask |= KEY_MOD_CTRL;
        else kb->modifier_mask &= ~KEY_MOD_CTRL;
    } else if (row == 3 && col == 2) {
        if (pressed) kb->modifier_mask |= KEY_MOD_ALT;
        else kb->modifier_mask &= ~KEY_MOD_ALT;
    } else if (row == 3 && col == 1) {
        if (pressed) kb->modifier_mask |= KEY_MOD_OPT;
        else kb->modifier_mask &= ~KEY_MOD_OPT;
    } else if (row == 2 && col == 1 && pressed) {
        kb->caps_lock_on = !kb->caps_lock_on;
        ESP_LOGD(TAG, "CapsLock toggled: %s", kb->caps_lock_on ? "ON" : "OFF");
    }
}

static tca8418_legacy_key_t map_legacy_key_code(uint8_t row, uint8_t col)
{
    if (row == 2 && col == 11) return TCA8418_KEY_UP;
    if (row == 3 && col == 11) return TCA8418_KEY_DOWN;
    if (row == 3 && col == 10) return TCA8418_KEY_LEFT;
    if (row == 3 && col == 12) return TCA8418_KEY_RIGHT;
    if (row == 2 && col == 13) return TCA8418_KEY_ENTER;
    return TCA8418_KEY_OTHER;
}

static tca8418_key_event_t map_key_event(tca8418_keyboard_t *kb, uint8_t row, uint8_t col, bool pressed)
{
    tca8418_key_event_t event;
    event.pressed = pressed;
    event.is_modifier = false;
    event.key_code = KC_NONE;
    event.key_char = "";

    if (row >= 4 || col >= 14) return event;

    const key_value_t *kv = &KEY_MAP[row][col];
    event.key_code = kv->normal_code;

    if (event.key_code == KC_LSHIFT || event.key_code == KC_LCTRL ||
        event.key_code == KC_LALT || event.key_code == KC_LOPT ||
        event.key_code == KC_CAPSLOCK) {
        event.is_modifier = true;
        event.key_char = "";
        return event;
    }

    bool use_shifted = false;
    bool is_letter = (event.key_code >= KC_A && event.key_code <= KC_Z);

    if (is_letter) {
        bool shift_pressed = (kb->modifier_mask & KEY_MOD_SHIFT) != 0;
        use_shifted = shift_pressed != kb->caps_lock_on;
    } else {
        use_shifted = (kb->modifier_mask & KEY_MOD_SHIFT) != 0;
    }

    event.key_char = use_shifted ? kv->shifted : kv->normal;
    return event;
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    tca8418_keyboard_t *kb = (tca8418_keyboard_t *)arg;
    kb->isr_flag = true;
    BaseType_t woken = pdFALSE;
    if (kb->task_handle) {
        vTaskNotifyGiveFromISR(kb->task_handle, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void keyboard_task(void *arg)
{
    tca8418_keyboard_t *kb = (tca8418_keyboard_t *)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(5));

        for (int guard = 0; guard < 128; guard++) {
            uint8_t int_stat = i2c_device_read_reg(kb->i2c_dev, TCA8418_REG_INT_STAT);
            if ((int_stat & TCA8418_INT_STAT_K_INT) == 0) {
                kb->isr_flag = false;
                break;
            }

            uint8_t event = get_event(kb);
            if (event == 0) {
                i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_INT_STAT, 0x1F);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            bool pressed = (event & 0x80) != 0;
            uint8_t key_code = event & 0x7F;
            if (key_code == 0) continue;

            uint8_t raw_row = (key_code - 1) / 10;
            uint8_t raw_col = (key_code - 1) % 10;

            uint8_t row = raw_row;
            uint8_t col = raw_col;
            if (!remap_raw_key_to_logical(&row, &col)) {
                ESP_LOGD(TAG, "Ignored key: code=%d raw_row=%d raw_col=%d", key_code, raw_row, raw_col);
                continue;
            }

            const uint64_t mask = logical_key_mask(row, col);
            if (mask != 0) {
                const bool was_pressed = (kb->key_state_mask & mask) != 0;
                if (pressed == was_pressed) continue;
                if (pressed) kb->key_state_mask |= mask;
                else kb->key_state_mask &= ~mask;
            }

            ESP_LOGD(TAG, "Key %s: code=%d raw=(%d,%d) mapped=(%d,%d)",
                     pressed ? "pressed" : "released", key_code, raw_row, raw_col, row, col);

            update_modifier_state(kb, row, col, pressed);

            if (kb->key_event_callback) {
                tca8418_key_event_t key_event = map_key_event(kb, row, col, pressed);
                kb->key_event_callback(&key_event, kb->key_event_cb_ud);
            }

            if (pressed && kb->key_callback) {
                tca8418_legacy_key_t mapped_key = map_legacy_key_code(row, col);
                if (mapped_key != TCA8418_KEY_OTHER && mapped_key != TCA8418_KEY_NONE) {
                    kb->key_callback(mapped_key, kb->key_cb_ud);
                }
            }
        }

        i2c_device_write_reg(kb->i2c_dev, TCA8418_REG_INT_STAT, 0x1F);
    }
}

tca8418_keyboard_t *tca8418_keyboard_create(i2c_master_bus_handle_t i2c_bus, uint8_t addr, gpio_num_t int_pin)
{
    tca8418_keyboard_t *kb = calloc(1, sizeof(tca8418_keyboard_t));
    if (!kb) return NULL;

    kb->i2c_dev = i2c_device_create(i2c_bus, addr);
    kb->int_pin = int_pin;
    return kb;
}

void tca8418_keyboard_destroy(tca8418_keyboard_t *kb)
{
    if (!kb) return;
    if (kb->task_handle) {
        vTaskDelete(kb->task_handle);
    }
    gpio_isr_handler_remove(kb->int_pin);
    if (kb->i2c_dev) i2c_device_destroy(kb->i2c_dev);
    free(kb);
}

void tca8418_keyboard_initialize(tca8418_keyboard_t *kb)
{
    ESP_LOGI(TAG, "Initializing TCA8418 keyboard");

    configure_matrix(kb);
    flush_events(kb);
    enable_interrupts(kb);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << kb->int_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(kb->int_pin, gpio_isr_handler, kb);

    xTaskCreate(keyboard_task, "keyboard_task", 4096, kb, 5, &kb->task_handle);

    ESP_LOGI(TAG, "TCA8418 keyboard initialized");
}

void tca8418_keyboard_set_key_cb(tca8418_keyboard_t *kb, tca8418_key_cb_t cb, void *ud)
{
    kb->key_callback = cb;
    kb->key_cb_ud = ud;
}

void tca8418_keyboard_set_key_event_cb(tca8418_keyboard_t *kb, tca8418_key_event_cb_t cb, void *ud)
{
    kb->key_event_callback = cb;
    kb->key_event_cb_ud = ud;
}

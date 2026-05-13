#include "display/display.h"

#include <stdbool.h>
#include <stdint.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <string.h>
#include <stdlib.h>
#include <lvgl.h>

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

typedef struct {
    bool pressed;
    bool is_modifier;
    uint8_t key_code;
    const char *key_char;
} tca8418_key_event_t;

typedef enum {
    WIFI_CFG_STATE_SCANNING,
    WIFI_CFG_STATE_SELECT_WIFI,
    WIFI_CFG_STATE_INPUT_PASSWORD,
    WIFI_CFG_STATE_INPUT_SSID,
    WIFI_CFG_STATE_INPUT_MANUAL_PWD,
    WIFI_CFG_STATE_SAVED_LIST,
    WIFI_CFG_STATE_CONNECTING,
    WIFI_CFG_STATE_SUCCESS,
    WIFI_CFG_STATE_FAILED,
} wifi_config_state_t;

typedef enum {
    WIFI_CFG_RESULT_NONE,
    WIFI_CFG_RESULT_CONNECTED,
    WIFI_CFG_RESULT_CANCELLED,
} wifi_config_result_t;

typedef struct wifi_config_ui wifi_config_ui_t;
typedef void (*wifi_config_connect_cb_t)(const char *ssid, const char *password, void *ud);

wifi_config_ui_t *wifi_config_ui_create(display_t *display);
void wifi_config_ui_destroy(wifi_config_ui_t *ui);
void wifi_config_ui_set_connect_cb(wifi_config_ui_t *ui, wifi_config_connect_cb_t cb, void *ud);
void wifi_config_ui_start(wifi_config_ui_t *ui);
void wifi_config_ui_start_with_saved(wifi_config_ui_t *ui);
void wifi_config_ui_on_connect_result(wifi_config_ui_t *ui, bool success);
bool wifi_config_ui_is_active(wifi_config_ui_t *ui);
wifi_config_result_t wifi_config_ui_handle_key(wifi_config_ui_t *ui, const tca8418_key_event_t *event);
void wifi_config_ui_update_cursor(wifi_config_ui_t *ui);

#define TAG "WifiConfigUI"

#define MAX_VISIBLE_ITEMS  4
#define MAX_INPUT_LENGTH   64
#define CURSOR_BLINK_MS    500
#define MAX_SCAN_RESULTS   20
#define MAX_SAVED_WIFI     10

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool is_encrypted;
} wifi_scan_entry_t;

typedef struct {
    char ssid[33];
    char password[65];
} saved_wifi_entry_t;

struct wifi_config_ui {
    display_t *display;
    wifi_config_state_t state;
    bool is_active;

    wifi_config_connect_cb_t connect_callback;
    void *connect_cb_ud;

    wifi_scan_entry_t scan_results[MAX_SCAN_RESULTS];
    int scan_count;
    int selected_index;
    int scroll_offset;

    saved_wifi_entry_t saved_wifi[MAX_SAVED_WIFI];
    int saved_count;
    int saved_selected_index;
    int saved_scroll_offset;

    char input_ssid[MAX_INPUT_LENGTH + 1];
    char input_password[MAX_INPUT_LENGTH + 1];
    char selected_ssid[33];
    bool input_focus_on_password;

    bool cursor_visible;
    uint32_t last_cursor_toggle;
};

static void draw_header(const char *title)
{
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_t *header = lv_label_create(canvas);
    lv_label_set_text(header, title);
    lv_obj_set_style_text_color(header, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 5, 2);
}

static void draw_footer(const char *hint)
{
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_t *footer = lv_label_create(canvas);
    lv_label_set_text(footer, hint);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 5, -2);
}

static const char *get_signal_bars(int8_t rssi)
{
    if (rssi >= -50) return "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88";
    if (rssi >= -60) return "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91";
    if (rssi >= -70) return "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91";
    if (rssi >= -80) return "\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91";
    return "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91";
}

static void load_saved_wifi_list(wifi_config_ui_t *ui)
{
    ui->saved_count = 0;
    ssid_list_t *list = ssid_manager_get_list();
    if (!list) return;
    int n = ssid_list_count(list);
    for (int i = 0; i < n && i < MAX_SAVED_WIFI; i++) {
        const char *ssid = ssid_list_get_ssid(list, i);
        const char *pwd = ssid_list_get_password(list, i);
        if (ssid) {
            strncpy(ui->saved_wifi[ui->saved_count].ssid, ssid, 32);
            ui->saved_wifi[ui->saved_count].ssid[32] = '\0';
        }
        if (pwd) {
            strncpy(ui->saved_wifi[ui->saved_count].password, pwd, 64);
            ui->saved_wifi[ui->saved_count].password[64] = '\0';
        }
        ui->saved_count++;
    }
}

static void do_wifi_scan(wifi_config_ui_t *ui)
{
    ui->scan_count = 0;

    wifi_scan_config_t scan_config = {0};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (!ap_records) return;
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);

        for (int i = 0; i < ap_count && ui->scan_count < MAX_SCAN_RESULTS; i++) {
            if (ap_records[i].ssid[0] == '\0') continue;
            strncpy(ui->scan_results[ui->scan_count].ssid, (char *)ap_records[i].ssid, 32);
            ui->scan_results[ui->scan_count].ssid[32] = '\0';
            ui->scan_results[ui->scan_count].rssi = ap_records[i].rssi;
            ui->scan_results[ui->scan_count].is_encrypted = (ap_records[i].authmode != WIFI_AUTH_OPEN);
            ui->scan_count++;
        }

        free(ap_records);
    }

    ESP_LOGI(TAG, "Found %d WiFi networks", ui->scan_count);
}

static void draw_wifi_list(wifi_config_ui_t *ui)
{
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe9\x80\x89\xe6\x8b\xa9 WiFi");

    int y_offset = 25;
    int visible = ui->scan_count - ui->scroll_offset;
    if (visible > MAX_VISIBLE_ITEMS) visible = MAX_VISIBLE_ITEMS;

    for (int i = 0; i < visible; i++) {
        int idx = ui->scroll_offset + i;
        bool is_sel = (idx == ui->selected_index);
        wifi_scan_entry_t *w = &ui->scan_results[idx];

        lv_obj_t *label = lv_label_create(canvas);
        char buf[80];
        char ssid_short[13];
        strncpy(ssid_short, w->ssid, 12);
        ssid_short[12] = '\0';
        snprintf(buf, sizeof(buf), "%s%d.%-12s %4ddBm %s",
                 is_sel ? ">" : " ", idx + 1, ssid_short, w->rssi, get_signal_bars(w->rssi));
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_color(label, is_sel ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 2, y_offset);
        y_offset += 20;
    }

    draw_footer("\xe2\x86\x91\xe2\x86\x93:\xe9\x80\x89\xe6\x8b\xa9 Enter:\xe8\xbf\x9e\xe6\x8e\xa5 W:\xe6\x89\x8b\xe5\x8a\xa8 S:\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98");
}

static void show_scan_results(wifi_config_ui_t *ui)
{
    draw_wifi_list(ui);
}

static void redraw_password_input(wifi_config_ui_t *ui)
{
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe8\xbe\x93\xe5\x85\xa5\xe5\xaf\x86\xe7\xa0\x81");

    lv_obj_t *lbl = lv_label_create(canvas);
    char buf[64];
    snprintf(buf, sizeof(buf), "\xe8\xbf\x9e\xe6\x8e\xa5: %s", ui->selected_ssid);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 5, 5);

    lv_obj_t *pwd_lbl = lv_label_create(canvas);
    lv_label_set_text(pwd_lbl, "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5\xe5\xaf\x86\xe7\xa0\x81:");
    lv_obj_set_style_text_color(pwd_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(pwd_lbl, LV_ALIGN_TOP_LEFT, 5, 30);

    lv_obj_t *input_lbl = lv_label_create(canvas);
    size_t plen = strlen(ui->input_password);
    char stars[MAX_INPUT_LENGTH + 8];
    memset(stars, '*', plen);
    stars[plen] = '\0';
    char display_buf[MAX_INPUT_LENGTH + 16];
    snprintf(display_buf, sizeof(display_buf), ">>> %s%s", stars, ui->cursor_visible ? "_" : " ");
    lv_label_set_text(input_lbl, display_buf);
    lv_obj_set_style_text_color(input_lbl, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(input_lbl, LV_ALIGN_TOP_LEFT, 5, 55);

    draw_footer("Enter:\xe7\xa1\xae\xe8\xae\xa4 Esc:\xe8\xbf\x94\xe5\x9b\x9e");
}

static void show_password_input(wifi_config_ui_t *ui)
{
    if (ui->state != WIFI_CFG_STATE_INPUT_PASSWORD) {
        ui->state = WIFI_CFG_STATE_INPUT_PASSWORD;
        ui->input_password[0] = '\0';
    }
    redraw_password_input(ui);
}

static void redraw_manual_input(wifi_config_ui_t *ui)
{
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe6\x89\x8b\xe5\x8a\xa8\xe8\xae\xbe\xe7\xbd\xae WiFi");

    lv_obj_t *ssid_lbl = lv_label_create(canvas);
    lv_label_set_text(ssid_lbl, "SSID:");
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 5, 25);

    lv_obj_t *ssid_input = lv_label_create(canvas);
    char buf[MAX_INPUT_LENGTH + 16];
    snprintf(buf, sizeof(buf), ">>> %s%s", ui->input_ssid,
             !ui->input_focus_on_password ? (ui->cursor_visible ? "_" : " ") : "");
    lv_label_set_text(ssid_input, buf);
    lv_obj_set_style_text_color(ssid_input,
        ui->input_focus_on_password ? lv_color_hex(0x888888) : lv_color_hex(0xFFFF00), 0);
    lv_obj_align(ssid_input, LV_ALIGN_TOP_LEFT, 5, 45);

    lv_obj_t *pwd_lbl = lv_label_create(canvas);
    lv_label_set_text(pwd_lbl, "\xe5\xaf\x86\xe7\xa0\x81:");
    lv_obj_set_style_text_color(pwd_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(pwd_lbl, LV_ALIGN_TOP_LEFT, 5, 70);

    lv_obj_t *pwd_input = lv_label_create(canvas);
    size_t plen = strlen(ui->input_password);
    char stars[MAX_INPUT_LENGTH + 8];
    memset(stars, '*', plen);
    stars[plen] = '\0';
    snprintf(buf, sizeof(buf), ">>> %s%s", stars,
             ui->input_focus_on_password ? (ui->cursor_visible ? "_" : " ") : "");
    lv_label_set_text(pwd_input, buf);
    lv_obj_set_style_text_color(pwd_input,
        ui->input_focus_on_password ? lv_color_hex(0xFFFF00) : lv_color_hex(0x888888), 0);
    lv_obj_align(pwd_input, LV_ALIGN_TOP_LEFT, 5, 90);

    draw_footer("Tab:\xe5\x88\x87\xe6\x8d\xa2 Enter:\xe7\xa1\xae\xe8\xae\xa4 Esc:\xe8\xbf\x94\xe5\x9b\x9e");
}

static void show_manual_input(wifi_config_ui_t *ui)
{
    if (ui->state != WIFI_CFG_STATE_INPUT_SSID && ui->state != WIFI_CFG_STATE_INPUT_MANUAL_PWD) {
        ui->state = WIFI_CFG_STATE_INPUT_SSID;
        ui->input_ssid[0] = '\0';
        ui->input_password[0] = '\0';
        ui->input_focus_on_password = false;
    }
    redraw_manual_input(ui);
}

static void draw_saved_wifi_list(wifi_config_ui_t *ui)
{
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);

    char title[48];
    snprintf(title, sizeof(title), "\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xe7\x9a\x84 WiFi (%d/10)", ui->saved_count);
    draw_header(title);

    if (ui->saved_count == 0) {
        lv_obj_t *empty = lv_label_create(canvas);
        lv_label_set_text(empty, "\xe6\xb2\xa1\xe6\x9c\x89\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xe7\x9a\x84 WiFi");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        draw_footer("Esc:\xe8\xbf\x94\xe5\x9b\x9e");
        return;
    }

    int y_offset = 25;
    int visible = ui->saved_count - ui->saved_scroll_offset;
    if (visible > MAX_VISIBLE_ITEMS) visible = MAX_VISIBLE_ITEMS;

    for (int i = 0; i < visible; i++) {
        int idx = ui->saved_scroll_offset + i;
        bool is_sel = (idx == ui->saved_selected_index);
        lv_obj_t *label = lv_label_create(canvas);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %d. %s",
                 is_sel ? ">" : " ", idx + 1, ui->saved_wifi[idx].ssid);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_color(label, is_sel ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 5, y_offset);
        y_offset += 20;
    }

    draw_footer("\xe2\x86\x91\xe2\x86\x93:\xe9\x80\x89\xe6\x8b\xa9 Enter:\xe8\xbf\x9e\xe6\x8e\xa5 Del:\xe5\x88\xa0\xe9\x99\xa4 Esc:\xe8\xbf\x94\xe5\x9b\x9e");
}

static void show_saved_list(wifi_config_ui_t *ui)
{
    ui->state = WIFI_CFG_STATE_SAVED_LIST;
    ui->saved_selected_index = 0;
    ui->saved_scroll_offset = 0;
    load_saved_wifi_list(ui);
    draw_saved_wifi_list(ui);
}

static void show_connecting(wifi_config_ui_t *ui)
{
    ui->state = WIFI_CFG_STATE_CONNECTING;
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe8\xbf\x9e\xe6\x8e\xa5\xe4\xb8\xad...");
    lv_obj_t *lbl = lv_label_create(canvas);
    char buf[64];
    snprintf(buf, sizeof(buf), "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5: %s", ui->selected_ssid);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    draw_footer("\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99...");
}

static void show_success(wifi_config_ui_t *ui)
{
    ui->state = WIFI_CFG_STATE_SUCCESS;
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x88\x90\xe5\x8a\x9f!");
    lv_obj_t *lbl = lv_label_create(canvas);
    char buf[64];
    snprintf(buf, sizeof(buf), "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5: %s", ui->selected_ssid);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF00), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *saved = lv_label_create(canvas);
    lv_label_set_text(saved, "WiFi \xe9\x85\x8d\xe7\xbd\xae\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98");
    lv_obj_set_style_text_color(saved, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(saved, LV_ALIGN_CENTER, 0, 15);
    draw_footer("Enter:\xe7\xbb\xa7\xe7\xbb\xad");
}

static void show_failed(wifi_config_ui_t *ui)
{
    ui->state = WIFI_CFG_STATE_FAILED;
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5");
    lv_obj_t *lbl = lv_label_create(canvas);
    char buf[64];
    snprintf(buf, sizeof(buf), "\xe6\x97\xa0\xe6\xb3\x95\xe8\xbf\x9e\xe6\x8e\xa5: %s", ui->selected_ssid);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF0000), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    draw_footer("Enter:\xe9\x87\x8d\xe8\xaf\x95 Esc:\xe8\xbf\x94\xe5\x9b\x9e");
}

static void attempt_connection(wifi_config_ui_t *ui)
{
    show_connecting(ui);
    if (ui->connect_callback) {
        ui->connect_callback(ui->selected_ssid, ui->input_password, ui->connect_cb_ud);
    }
}

static void save_wifi_credentials(wifi_config_ui_t *ui)
{
    ssid_manager_add(ui->selected_ssid, ui->input_password);
    ESP_LOGI(TAG, "Saved WiFi credentials for: %s", ui->selected_ssid);
}

static void delete_saved_wifi(wifi_config_ui_t *ui, int index)
{
    if (index >= 0 && index < ui->saved_count) {
        ssid_manager_remove(index);
        ESP_LOGI(TAG, "Deleted saved WiFi at index: %d", index);
        load_saved_wifi_list(ui);
    }
}

wifi_config_ui_t *wifi_config_ui_create(display_t *display)
{
    wifi_config_ui_t *ui = calloc(1, sizeof(wifi_config_ui_t));
    if (!ui) return NULL;
    ui->display = display;
    ui->state = WIFI_CFG_STATE_SCANNING;
    ui->cursor_visible = true;
    return ui;
}

void wifi_config_ui_destroy(wifi_config_ui_t *ui)
{
    free(ui);
}

void wifi_config_ui_set_connect_cb(wifi_config_ui_t *ui, wifi_config_connect_cb_t cb, void *ud)
{
    ui->connect_callback = cb;
    ui->connect_cb_ud = ud;
}

void wifi_config_ui_start(wifi_config_ui_t *ui)
{
    ESP_LOGI(TAG, "Starting WiFi config UI");
    ui->is_active = true;
    ui->state = WIFI_CFG_STATE_SCANNING;
    ui->selected_index = 0;
    ui->scroll_offset = 0;
    ui->input_ssid[0] = '\0';
    ui->input_password[0] = '\0';
    ui->selected_ssid[0] = '\0';

    load_saved_wifi_list(ui);

    ui->state = WIFI_CFG_STATE_SCANNING;
    lv_obj_t *canvas = lv_scr_act();
    lv_obj_clean(canvas);
    draw_header("\xe6\x89\xab\xe6\x8f\x8f WiFi \xe4\xb8\xad...");
    draw_footer("\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99...");

    do_wifi_scan(ui);

    if (ui->scan_count == 0) {
        lv_obj_clean(canvas);
        draw_header("\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0 WiFi");
        draw_footer("W:\xe6\x89\x8b\xe5\x8a\xa8\xe8\xbe\x93\xe5\x85\xa5 Esc:\xe9\x80\x80\xe5\x87\xba");
    } else {
        ui->state = WIFI_CFG_STATE_SELECT_WIFI;
        show_scan_results(ui);
    }
}

void wifi_config_ui_start_with_saved(wifi_config_ui_t *ui)
{
    ESP_LOGI(TAG, "Starting WiFi config UI with saved list");
    ui->is_active = true;
    ui->selected_index = 0;
    ui->scroll_offset = 0;
    ui->input_ssid[0] = '\0';
    ui->input_password[0] = '\0';
    ui->selected_ssid[0] = '\0';
    show_saved_list(ui);
}

void wifi_config_ui_on_connect_result(wifi_config_ui_t *ui, bool success)
{
    if (success) {
        save_wifi_credentials(ui);
        show_success(ui);
    } else {
        show_failed(ui);
    }
}

bool wifi_config_ui_is_active(wifi_config_ui_t *ui)
{
    return ui->is_active;
}

wifi_config_result_t wifi_config_ui_handle_key(wifi_config_ui_t *ui, const tca8418_key_event_t *event)
{
    if (!event->pressed || event->is_modifier) return WIFI_CFG_RESULT_NONE;

    if (event->key_code == KC_ESC) {
        if (ui->state == WIFI_CFG_STATE_SCANNING || ui->state == WIFI_CFG_STATE_SELECT_WIFI) {
            ui->is_active = false;
            return WIFI_CFG_RESULT_CANCELLED;
        }
    }

    if (!ui->is_active) return WIFI_CFG_RESULT_CANCELLED;

    switch (ui->state) {
    case WIFI_CFG_STATE_SCANNING:
        if (event->key_code == KC_W) show_manual_input(ui);
        else if (event->key_code == KC_S) show_saved_list(ui);
        break;

    case WIFI_CFG_STATE_SELECT_WIFI:
        switch (event->key_code) {
        case KC_UP: case KC_SEMICOLON:
            if (ui->selected_index > 0) {
                ui->selected_index--;
                if (ui->selected_index < ui->scroll_offset)
                    ui->scroll_offset = ui->selected_index;
                show_scan_results(ui);
            }
            break;
        case KC_DOWN: case KC_DOT:
            if (ui->selected_index < ui->scan_count - 1) {
                ui->selected_index++;
                if (ui->selected_index >= ui->scroll_offset + MAX_VISIBLE_ITEMS)
                    ui->scroll_offset = ui->selected_index - MAX_VISIBLE_ITEMS + 1;
                show_scan_results(ui);
            }
            break;
        case KC_ENTER:
            if (ui->scan_count > 0) {
                strncpy(ui->selected_ssid, ui->scan_results[ui->selected_index].ssid, 32);
                ui->selected_ssid[32] = '\0';
                show_password_input(ui);
            }
            break;
        case KC_W: show_manual_input(ui); break;
        case KC_S: show_saved_list(ui); break;
        default: break;
        }
        break;

    case WIFI_CFG_STATE_INPUT_PASSWORD:
        switch (event->key_code) {
        case KC_ENTER:
            if (ui->input_password[0] != '\0') attempt_connection(ui);
            break;
        case KC_ESC:
            ui->state = WIFI_CFG_STATE_SELECT_WIFI;
            show_scan_results(ui);
            break;
        case KC_BACKSPACE: {
            size_t len = strlen(ui->input_password);
            if (len > 0) { ui->input_password[len - 1] = '\0'; redraw_password_input(ui); }
            break;
        }
        case KC_SPACE:
            if (strlen(ui->input_password) < MAX_INPUT_LENGTH) {
                size_t len = strlen(ui->input_password);
                ui->input_password[len] = ' ';
                ui->input_password[len + 1] = '\0';
                redraw_password_input(ui);
            }
            break;
        default:
            if (event->key_char && event->key_char[0] != '\0' && strlen(ui->input_password) < MAX_INPUT_LENGTH) {
                size_t len = strlen(ui->input_password);
                size_t clen = strlen(event->key_char);
                if (len + clen <= MAX_INPUT_LENGTH) {
                    strcat(ui->input_password, event->key_char);
                    redraw_password_input(ui);
                }
            }
            break;
        }
        break;

    case WIFI_CFG_STATE_INPUT_SSID:
    case WIFI_CFG_STATE_INPUT_MANUAL_PWD: {
        char *current = ui->input_focus_on_password ? ui->input_password : ui->input_ssid;
        switch (event->key_code) {
        case KC_TAB:
            ui->input_focus_on_password = !ui->input_focus_on_password;
            ui->state = ui->input_focus_on_password ? WIFI_CFG_STATE_INPUT_MANUAL_PWD : WIFI_CFG_STATE_INPUT_SSID;
            redraw_manual_input(ui);
            break;
        case KC_ENTER:
            if (ui->input_ssid[0] != '\0') {
                strncpy(ui->selected_ssid, ui->input_ssid, 32);
                ui->selected_ssid[32] = '\0';
                attempt_connection(ui);
            }
            break;
        case KC_ESC:
            ui->state = WIFI_CFG_STATE_SELECT_WIFI;
            show_scan_results(ui);
            break;
        case KC_BACKSPACE: {
            size_t len = strlen(current);
            if (len > 0) { current[len - 1] = '\0'; redraw_manual_input(ui); }
            break;
        }
        case KC_SPACE:
            if (strlen(current) < MAX_INPUT_LENGTH) {
                size_t len = strlen(current);
                current[len] = ' ';
                current[len + 1] = '\0';
                redraw_manual_input(ui);
            }
            break;
        default:
            if (event->key_char && event->key_char[0] != '\0' && strlen(current) < MAX_INPUT_LENGTH) {
                strcat(current, event->key_char);
                redraw_manual_input(ui);
            }
            break;
        }
        break;
    }

    case WIFI_CFG_STATE_SAVED_LIST:
        switch (event->key_code) {
        case KC_UP: case KC_SEMICOLON:
            if (ui->saved_selected_index > 0) {
                ui->saved_selected_index--;
                if (ui->saved_selected_index < ui->saved_scroll_offset)
                    ui->saved_scroll_offset = ui->saved_selected_index;
                draw_saved_wifi_list(ui);
            }
            break;
        case KC_DOWN: case KC_DOT:
            if (ui->saved_selected_index < ui->saved_count - 1) {
                ui->saved_selected_index++;
                if (ui->saved_selected_index >= ui->saved_scroll_offset + MAX_VISIBLE_ITEMS)
                    ui->saved_scroll_offset = ui->saved_selected_index - MAX_VISIBLE_ITEMS + 1;
                draw_saved_wifi_list(ui);
            }
            break;
        case KC_ENTER:
            if (ui->saved_count > 0) {
                strncpy(ui->selected_ssid, ui->saved_wifi[ui->saved_selected_index].ssid, 32);
                ui->selected_ssid[32] = '\0';
                strncpy(ui->input_password, ui->saved_wifi[ui->saved_selected_index].password, MAX_INPUT_LENGTH);
                ui->input_password[MAX_INPUT_LENGTH] = '\0';
                attempt_connection(ui);
            }
            break;
        case KC_BACKSPACE:
            if (ui->saved_count > 0) {
                delete_saved_wifi(ui, ui->saved_selected_index);
                if (ui->saved_selected_index >= ui->saved_count && ui->saved_selected_index > 0)
                    ui->saved_selected_index--;
                draw_saved_wifi_list(ui);
            }
            break;
        case KC_ESC:
            ui->state = WIFI_CFG_STATE_SELECT_WIFI;
            show_scan_results(ui);
            break;
        default: break;
        }
        break;

    case WIFI_CFG_STATE_CONNECTING:
        break;

    case WIFI_CFG_STATE_SUCCESS:
        if (event->key_code == KC_ENTER) {
            ui->is_active = false;
            return WIFI_CFG_RESULT_CONNECTED;
        }
        break;

    case WIFI_CFG_STATE_FAILED:
        if (event->key_code == KC_ENTER) {
            ui->state = WIFI_CFG_STATE_INPUT_PASSWORD;
            redraw_password_input(ui);
        } else if (event->key_code == KC_ESC) {
            ui->state = WIFI_CFG_STATE_SELECT_WIFI;
            show_scan_results(ui);
        }
        break;
    }

    if (!ui->is_active) return WIFI_CFG_RESULT_CANCELLED;
    return WIFI_CFG_RESULT_NONE;
}

void wifi_config_ui_update_cursor(wifi_config_ui_t *ui)
{
    uint32_t now = esp_log_timestamp();
    if (now - ui->last_cursor_toggle >= CURSOR_BLINK_MS) {
        ui->cursor_visible = !ui->cursor_visible;
        ui->last_cursor_toggle = now;

        if (ui->state == WIFI_CFG_STATE_INPUT_PASSWORD) {
            redraw_password_input(ui);
        } else if (ui->state == WIFI_CFG_STATE_INPUT_SSID || ui->state == WIFI_CFG_STATE_INPUT_MANUAL_PWD) {
            redraw_manual_input(ui);
        }
    }
}

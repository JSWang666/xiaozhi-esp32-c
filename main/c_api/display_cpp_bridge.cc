#include "display.h"
#include "oled_display.h"
#include "lcd_display.h"

#include <new>

struct display_wrapper {
    display_t base;
    Display *impl;
};

static void disp_set_status(display_t *d, const char *s) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->SetStatus(s);
}

static void disp_show_notification(display_t *d, const char *t, int ms) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->ShowNotification(t, ms);
}

static void disp_set_emotion(display_t *d, const char *e) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->SetEmotion(e);
}

static void disp_set_chat_message(display_t *d, const char *role, const char *content) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->SetChatMessage(role, content);
}

static void disp_clear_chat(display_t *d) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->ClearChatMessages();
}

static void disp_setup_ui(display_t *d) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->SetupUI();
}

static void disp_update_status_bar(display_t *d, bool update_all) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->UpdateStatusBar(update_all);
}

static void disp_set_power_save(display_t *d, bool on) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    if (w->impl) w->impl->SetPowerSaveMode(on);
}

static bool disp_lock(display_t *d, int timeout_ms) {
    (void)d; (void)timeout_ms;
    return true;
}

static void disp_unlock(display_t *d) {
    (void)d;
}

static void disp_destroy(display_t *d) {
    auto *w = reinterpret_cast<display_wrapper *>(d);
    delete w->impl;
    delete w;
}

static const display_ops_t cpp_display_ops = {
    .set_status = disp_set_status,
    .show_notification = disp_show_notification,
    .set_emotion = disp_set_emotion,
    .set_chat_message = disp_set_chat_message,
    .clear_chat_messages = disp_clear_chat,
    .setup_ui = disp_setup_ui,
    .update_status_bar = disp_update_status_bar,
    .set_power_save_mode = disp_set_power_save,
    .lock = disp_lock,
    .unlock = disp_unlock,
    .destroy = disp_destroy,
};

static display_t *wrap_cpp_display(Display *impl) {
    if (!impl) return nullptr;
    auto *w = new (std::nothrow) display_wrapper{};
    if (!w) { delete impl; return nullptr; }
    w->impl = impl;
    w->base.ops = &cpp_display_ops;
    w->base.width = impl->width();
    w->base.height = impl->height();
    w->base.setup_ui_called = impl->IsSetupUICalled();
    return &w->base;
}

/* Unwraps a display_t* that was created via this bridge and returns
 * the underlying C++ Display*. Returns nullptr if d is null or was not
 * created by this bridge (i.e. ops != cpp_display_ops). */
Display *display_unwrap_cpp(display_t *d) {
    if (!d) return nullptr;
    if (d->ops != &cpp_display_ops) return nullptr;
    auto *w = reinterpret_cast<display_wrapper *>(d);
    return w->impl;
}

extern "C" {

display_t *oled_display_create(void *panel_io, void *panel,
    int width, int height, bool mirror_x, bool mirror_y)
{
    auto *impl = new (std::nothrow) OledDisplay(
        (esp_lcd_panel_io_handle_t)panel_io,
        (esp_lcd_panel_handle_t)panel,
        width, height, mirror_x, mirror_y);
    return wrap_cpp_display(impl);
}

display_t *spi_lcd_display_create(void *panel_io, void *panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
{
    auto *impl = new (std::nothrow) SpiLcdDisplay(
        (esp_lcd_panel_io_handle_t)panel_io,
        (esp_lcd_panel_handle_t)panel,
        width, height, offset_x, offset_y,
        mirror_x, mirror_y, swap_xy);
    return wrap_cpp_display(impl);
}

display_t *rgb_lcd_display_create(void *panel_io, void *panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
{
    auto *impl = new (std::nothrow) RgbLcdDisplay(
        (esp_lcd_panel_io_handle_t)panel_io,
        (esp_lcd_panel_handle_t)panel,
        width, height, offset_x, offset_y,
        mirror_x, mirror_y, swap_xy);
    return wrap_cpp_display(impl);
}

display_t *mipi_lcd_display_create(void *panel_io, void *panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy)
{
    auto *impl = new (std::nothrow) MipiLcdDisplay(
        (esp_lcd_panel_io_handle_t)panel_io,
        (esp_lcd_panel_handle_t)panel,
        width, height, offset_x, offset_y,
        mirror_x, mirror_y, swap_xy);
    return wrap_cpp_display(impl);
}

}

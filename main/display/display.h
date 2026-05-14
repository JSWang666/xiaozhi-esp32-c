#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct display_t display_t;

typedef struct display_ops {
    void (*set_status)(display_t *disp, const char *status);
    void (*show_notification)(display_t *disp, const char *text, int duration_ms);
    void (*set_emotion)(display_t *disp, const char *emotion);
    void (*set_chat_message)(display_t *disp, const char *role, const char *content);
    void (*clear_chat_messages)(display_t *disp);
    void (*setup_ui)(display_t *disp);
    void (*update_status_bar)(display_t *disp, bool update_all);
    void (*set_power_save_mode)(display_t *disp, bool on);
    bool (*lock)(display_t *disp, int timeout_ms);
    void (*unlock)(display_t *disp);
    void (*destroy)(display_t *disp);
} display_ops_t;

struct display_t {
    const display_ops_t *ops;
    int width;
    int height;
    bool setup_ui_called;
};

display_t *no_display_create(void);

static inline void display_set_status(display_t *d, const char *s) {
    if (d && d->ops && d->ops->set_status) d->ops->set_status(d, s);
}
static inline void display_show_notification(display_t *d, const char *t, int ms) {
    if (d && d->ops && d->ops->show_notification) d->ops->show_notification(d, t, ms);
}
static inline void display_set_emotion(display_t *d, const char *e) {
    if (d && d->ops && d->ops->set_emotion) d->ops->set_emotion(d, e);
}
static inline void display_set_chat_message(display_t *d, const char *role, const char *content) {
    if (d && d->ops && d->ops->set_chat_message) d->ops->set_chat_message(d, role, content);
}
static inline void display_clear_chat_messages(display_t *d) {
    if (d && d->ops && d->ops->clear_chat_messages) d->ops->clear_chat_messages(d);
}
static inline bool display_lock(display_t *d, int timeout_ms) {
    if (d && d->ops && d->ops->lock) return d->ops->lock(d, timeout_ms);
    return false;
}
static inline void display_unlock(display_t *d) {
    if (d && d->ops && d->ops->unlock) d->ops->unlock(d);
}
static inline void display_setup_ui(display_t *d) {
    if (d && d->ops && d->ops->setup_ui) d->ops->setup_ui(d);
}
static inline void display_update_status_bar(display_t *d, bool update_all) {
    if (d && d->ops && d->ops->update_status_bar) d->ops->update_status_bar(d, update_all);
}
static inline void display_set_power_save_mode(display_t *d, bool on) {
    if (d && d->ops && d->ops->set_power_save_mode) d->ops->set_power_save_mode(d, on);
}
static inline void display_destroy(display_t *d) {
    if (d && d->ops && d->ops->destroy) d->ops->destroy(d);
}

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "emoji_collection.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;
    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual void SetupUI() {
        setup_ui_called_ = true;
    }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

    display_t *c_display() { return c_display_; }

    /** LVGL / cross-thread mutual exclusion; used by DisplayLockGuard and C display_ops. */
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;
    display_t *c_display_ = nullptr;

    Theme* current_theme_ = nullptr;
};

class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }
private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif

#endif /* DISPLAY_H */

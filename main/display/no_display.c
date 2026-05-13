#include "display.h"
#include <stdlib.h>

static void no_display_setup_ui(display_t *d) {
    d->setup_ui_called = true;
}

static bool no_display_lock(display_t *d, int timeout_ms) {
    (void)d; (void)timeout_ms;
    return true;
}

static void no_display_unlock(display_t *d) {
    (void)d;
}

static void no_display_destroy(display_t *d) {
    free(d);
}

static const display_ops_t no_display_ops = {
    .set_status = NULL,
    .show_notification = NULL,
    .set_emotion = NULL,
    .set_chat_message = NULL,
    .clear_chat_messages = NULL,
    .setup_ui = no_display_setup_ui,
    .update_status_bar = NULL,
    .set_power_save_mode = NULL,
    .lock = no_display_lock,
    .unlock = no_display_unlock,
    .destroy = no_display_destroy,
};

display_t *no_display_create(void)
{
    display_t *d = (display_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->ops = &no_display_ops;
    return d;
}

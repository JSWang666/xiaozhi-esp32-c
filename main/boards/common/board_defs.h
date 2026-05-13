#ifndef BOARD_DEFS_H
#define BOARD_DEFS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct board_desc board_desc_t;

typedef enum {
    BOARD_KIND_WIFI = 0,
    BOARD_KIND_ML307,
    BOARD_KIND_NT26,
    BOARD_KIND_RNDIS,
    BOARD_KIND_DUAL,
} board_kind_t;

struct board_desc {
    board_kind_t kind;

    const char *(*get_board_type)(board_desc_t *self);

    void *(*get_led)(board_desc_t *self);
    void *(*get_audio_codec)(board_desc_t *self);
    void *(*get_display)(board_desc_t *self);
    void *(*get_backlight)(board_desc_t *self);
    void *(*get_camera)(board_desc_t *self);

    bool (*get_battery_level)(board_desc_t *self, int *level,
                              bool *charging, bool *discharging);
    bool (*get_temperature)(board_desc_t *self, float *temperature);

    void (*destroy)(board_desc_t *self);
};

board_desc_t *create_board_desc(void);

#ifdef __cplusplus
}
#endif

#endif

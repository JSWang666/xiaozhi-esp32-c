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

    /* Modem/network pin configuration, used only for non-Wi-Fi board kinds.
     * GPIO numbers are plain int so this header has no driver dependency;
     * pass GPIO_NUM_NC (-1) for unused pins. Default of 0 (left by calloc)
     * is _not_ valid — boards declaring ML307/NT26/DUAL MUST set these. */
    int modem_tx_pin;       /* ML307 / NT26 / DUAL */
    int modem_rx_pin;       /* ML307 / NT26 / DUAL */
    int modem_dtr_pin;      /* ML307 / NT26 / DUAL — may be GPIO_NUM_NC */
    int modem_ri_pin;       /* NT26 only — may be GPIO_NUM_NC */
    int modem_reset_pin;    /* NT26 only — may be GPIO_NUM_NC */
    int default_net_type;   /* DUAL only: 0 = Wi-Fi, 1 = ML307 */
};

board_desc_t *create_board_desc(void);

#ifdef __cplusplus
}
#endif

#endif

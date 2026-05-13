#ifndef DISPLAY_C_API_H
#define DISPLAY_C_API_H

#include "display.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

display_t *oled_display_create(void *panel_io, void *panel,
    int width, int height, bool mirror_x, bool mirror_y);

display_t *spi_lcd_display_create(void *panel_io, void *panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy);

display_t *rgb_lcd_display_create(void *panel_io, void *panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy);

display_t *mipi_lcd_display_create(void *panel_io, void *panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy);

#ifdef __cplusplus
}
#endif

#endif

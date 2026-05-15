#ifndef CST816X_H_
#define CST816X_H_

#include <stdint.h>
#include <driver/i2c_master.h>

#include "audio_codec.h"
#include "backlight.h"
#include "display/display.h"

#ifdef __cplusplus
extern "C" {
#endif

void cst816x_init(i2c_master_bus_handle_t i2c_bus, uint8_t addr,
                  audio_codec_t *codec, display_t *display, backlight_t *backlight);

#ifdef __cplusplus
}
#endif

#endif /* CST816X_H_ */

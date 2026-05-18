#ifndef SSCMA_CAMERA_API_H
#define SSCMA_CAMERA_API_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sscma_camera sscma_camera_t;

/**
 * Grab one JPEG frame from SSCMA (blocking). Copies into an internal buffer;
 * use sscma_camera_last_jpeg() to read it.
 */
esp_err_t sscma_camera_capture_still(sscma_camera_t *self, int timeout_ms);

/** JPEG from the last successful sscma_camera_capture_still (not freed by caller). */
const uint8_t *sscma_camera_last_jpeg(const sscma_camera_t *self, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif

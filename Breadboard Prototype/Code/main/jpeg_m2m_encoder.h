/* Stateful JPEG encoder lifecycle and caller-owned buffer contract. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin wrapper around esp_video's V4L2 memory-to-memory JPEG encoder.
 *
 * This follows the same ownership model used by r4d10n/esp32p4-uvc-video:
 *   OUTPUT  side: raw ISP frame supplied as V4L2_MEMORY_USERPTR
 *   CAPTURE side: encoded JPEG returned from a single MMAP buffer
 *
 * The returned JPEG pointer remains valid until jpeg_m2m_encoder_release() is
 * called.  Callers must write/copy the data first, then release the capture
 * buffer so the encoder can produce the next frame.
 */
typedef struct {
    int fd;
    uint8_t *capture_buffer;
    uint32_t capture_buf_size;
    uint32_t width;
    uint32_t height;
    uint32_t input_pixfmt;
    bool started;
} jpeg_m2m_encoder_t;

esp_err_t jpeg_m2m_encoder_open(jpeg_m2m_encoder_t *ctx, int quality);
esp_err_t jpeg_m2m_encoder_start(jpeg_m2m_encoder_t *ctx,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t input_pixfmt);
esp_err_t jpeg_m2m_encoder_encode(jpeg_m2m_encoder_t *ctx,
                                  uint8_t *raw_buf,
                                  uint32_t raw_len,
                                  uint8_t **jpeg_buf,
                                  uint32_t *jpeg_len);
esp_err_t jpeg_m2m_encoder_release(jpeg_m2m_encoder_t *ctx);
void jpeg_m2m_encoder_close(jpeg_m2m_encoder_t *ctx);

#ifdef __cplusplus
}
#endif

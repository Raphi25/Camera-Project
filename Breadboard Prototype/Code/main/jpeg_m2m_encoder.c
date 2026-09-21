/* V4L2 memory-to-memory wrapper around the ESP32-P4 hardware JPEG encoder. */

#include "jpeg_m2m_encoder.h"

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "linux/v4l2-controls.h"
#include "linux/videodev2.h"

static const char *TAG = "jpeg_m2m";

static esp_err_t set_jpeg_quality(int fd, int quality)
{
    struct v4l2_ext_control control = {
        .id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
        .value = quality,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_JPEG_CLASS,
        .count = 1,
        .controls = &control,
    };

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "JPEG quality control rejected; using driver default");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t jpeg_m2m_encoder_open(jpeg_m2m_encoder_t *ctx, int quality)
{
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_INVALID_ARG, TAG, "ctx is NULL");
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    ctx->fd = open(ESP_VIDEO_JPEG_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(ctx->fd >= 0, ESP_FAIL, TAG, "open %s failed", ESP_VIDEO_JPEG_DEVICE_NAME);

    struct v4l2_capability cap = {0};
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) == 0,
                        ESP_FAIL, TAG, "QUERYCAP failed");
    ESP_LOGI(TAG, "V4L2 JPEG M2M opened: %s (%s)", cap.card, cap.driver);

    (void)set_jpeg_quality(ctx->fd, quality);
    return ESP_OK;
}

esp_err_t jpeg_m2m_encoder_start(jpeg_m2m_encoder_t *ctx,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t input_pixfmt)
{
    ESP_RETURN_ON_FALSE(ctx && ctx->fd >= 0, ESP_ERR_INVALID_STATE, TAG, "encoder not open");
    if (ctx->started && ctx->width == width && ctx->height == height && ctx->input_pixfmt == input_pixfmt) {
        return ESP_OK;
    }
    if (ctx->started) {
        jpeg_m2m_encoder_close(ctx);
        ESP_RETURN_ON_ERROR(jpeg_m2m_encoder_open(ctx, 0), TAG, "reopen encoder");
    }

    ctx->width = width;
    ctx->height = height;
    ctx->input_pixfmt = input_pixfmt;

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix = {
            .width = width,
            .height = height,
            .pixelformat = input_pixfmt,
        },
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == 0,
                        ESP_FAIL, TAG, "S_FMT output failed");

    struct v4l2_requestbuffers req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "REQBUFS output failed");

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == 0,
                        ESP_FAIL, TAG, "S_FMT capture failed");

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "REQBUFS capture failed");

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = 0,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) == 0,
                        ESP_FAIL, TAG, "QUERYBUF capture failed");

    ctx->capture_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               ctx->fd, buf.m.offset);
    ESP_RETURN_ON_FALSE(ctx->capture_buffer != MAP_FAILED,
                        ESP_FAIL, TAG, "mmap capture failed");
    ctx->capture_buf_size = buf.length;

    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_QBUF, &buf) == 0,
                        ESP_FAIL, TAG, "QBUF capture failed");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "STREAMON capture failed");
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "STREAMON output failed");

    ctx->started = true;
    ESP_LOGI(TAG, "V4L2 JPEG M2M started: %" PRIu32 "x%" PRIu32 " input=0x%08" PRIx32,
             width, height, input_pixfmt);
    return ESP_OK;
}

esp_err_t jpeg_m2m_encoder_encode(jpeg_m2m_encoder_t *ctx,
                                  uint8_t *raw_buf,
                                  uint32_t raw_len,
                                  uint8_t **jpeg_buf,
                                  uint32_t *jpeg_len)
{
    ESP_RETURN_ON_FALSE(ctx && ctx->started, ESP_ERR_INVALID_STATE, TAG, "encoder not started");
    ESP_RETURN_ON_FALSE(raw_buf && raw_len && jpeg_buf && jpeg_len,
                        ESP_ERR_INVALID_ARG, TAG, "invalid encode args");

    struct v4l2_buffer out_buf = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
        .m.userptr = (unsigned long)raw_buf,
        .length = raw_len,
        .bytesused = raw_len,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_QBUF, &out_buf) == 0,
                        ESP_FAIL, TAG, "QBUF output failed");

    struct v4l2_buffer cap_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_DQBUF, &cap_buf) == 0,
                        ESP_FAIL, TAG, "DQBUF capture failed");

    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_DQBUF, &out_buf) == 0,
                        ESP_FAIL, TAG, "DQBUF output failed");

    *jpeg_buf = ctx->capture_buffer;
    *jpeg_len = cap_buf.bytesused;
    return ESP_OK;
}

esp_err_t jpeg_m2m_encoder_release(jpeg_m2m_encoder_t *ctx)
{
    ESP_RETURN_ON_FALSE(ctx && ctx->started, ESP_ERR_INVALID_STATE, TAG, "encoder not started");

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = 0,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->fd, VIDIOC_QBUF, &buf) == 0,
                        ESP_FAIL, TAG, "QBUF capture release failed");
    return ESP_OK;
}

void jpeg_m2m_encoder_close(jpeg_m2m_encoder_t *ctx)
{
    if (!ctx || ctx->fd < 0) {
        return;
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

    if (ctx->capture_buffer && ctx->capture_buffer != MAP_FAILED) {
        munmap(ctx->capture_buffer, ctx->capture_buf_size);
    }
    close(ctx->fd);
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
}

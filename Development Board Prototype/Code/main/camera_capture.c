/* Auto-detected MIPI-CSI/ISP capture with asynchronous, SD-safe persistence. */

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "device_credentials.h"
#include "daily_summary.h"
#include "image_crypto.h"
#include "media_transfer.h"
#include "psa/crypto.h"
#include "jpeg_m2m_encoder.h"
#include "linux/v4l2-controls.h"
#include "linux/videodev2.h"
#include "sd_storage.h"
#include "scene_classifier.h"
#include "watchdog_supervisor.h"

/*
 * Still-image camera pipeline.
 *
 * This module intentionally keeps a small state machine around esp_video:
 *   stream on  -> drain warmup frames until IPA/3A stabilizes
 *   dequeue    -> hold one completed frame
 *   stream off -> avoid CSI/ISP GDMA colliding with JPEG GDMA on ESP32-P4 rev 1
 *   encode     -> JPEG into a reusable output buffer
 *   queue job  -> writer task encrypts and writes while caller can continue
 *
 * The separation is a small embedded design pattern: interrupt/DMA-heavy work
 * stays bounded in the capture path, while slow storage is isolated behind a
 * queue.  Call camera_wait_for_idle() at power/serial protocol boundaries.
 */

static const char *TAG = "camera_capture";

/* The ISP pipeline retains one buffer internally while producing the next
 * frame. A zero-copy burst therefore needs one buffer per configured exposure
 * plus one spare so the final completed frame can be dequeued. */
#define VIDEO_BUFFER_COUNT (CAMERA_BURST_IMAGE_COUNT + 1U)
#define JPEG_OUT_BYTES (3 * 1024 * 1024)
/* A complete burst is encoded much faster than the encryption/SD worker can
 * persist it. Hold every JPEG so the final image is not dropped while earlier
 * burst files are still being encrypted. */
#define SAVE_QUEUE_LEN CAMERA_BURST_IMAGE_COUNT
#define WRITE_CHUNK_BYTES (64 * 1024)
#define VIDEO_DEQUEUE_TIMEOUT_MS 3000
/* After each stream restart the sensor/IPA needs a few frames to settle again.
 * Using the same count as the initial stream warmup lets us test whether the
 * dark/green frames are simply being captured too soon after each restart.
 */
/* Ownership transfer contract:
 * - camera_capture_once() allocates data in PSRAM and enqueues save_job_t.
 * - writer_task() always frees job.data, even on encryption/write failure.
 */
typedef struct {
    uint8_t *data;
    size_t size;
    char path[96];
    int64_t capture_start_us;
    struct tm captured_at;
    bool timestamp_valid;
    imu_orientation_sample_t orientation;
} save_job_t;

static int s_video_fd = -1;
static void *s_video_buf[VIDEO_BUFFER_COUNT];
static size_t s_video_buf_len[VIDEO_BUFFER_COUNT];
static uint32_t s_width;
static uint32_t s_height;
static uint32_t s_capture_fourcc;
#if CAMERA_JPEG_BACKEND_M2M
static jpeg_m2m_encoder_t s_m2m_jpeg;
#else
static jpeg_encoder_handle_t s_jpeg;
static uint8_t *s_jpeg_buf;
static size_t s_jpeg_buf_size;
#endif
static QueueHandle_t s_save_queue;
static TaskHandle_t s_writer_task;
static SemaphoreHandle_t s_capture_mutex;
static volatile bool s_writer_busy;
static volatile bool s_capture_paused;
static volatile bool s_burst_capture_active;
static bool s_streaming;
static bool s_buffers_queued;
static bool s_3a_settled;
static uint32_t s_sequence;
static scene_classifier_t s_scene_classifier;
static scene_stats_t s_scene_stats;
static esp_timer_handle_t s_capture_led_timer;

static void capture_led_off(void *arg)
{
    (void)arg;
    gpio_set_level(CAMERA_USER_LED_GPIO, !CAMERA_USER_LED_ACTIVE_LEVEL);
}

static esp_err_t capture_led_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CAMERA_USER_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure capture LED");
    gpio_set_level(CAMERA_USER_LED_GPIO, !CAMERA_USER_LED_ACTIVE_LEVEL);
    const esp_timer_create_args_t timer_config = {
        .callback = capture_led_off,
        .name = "capture_led",
    };
    return esp_timer_create(&timer_config, &s_capture_led_timer);
}

static void capture_led_blink(void)
{
    if (s_capture_led_timer == NULL) return;
    (void)esp_timer_stop(s_capture_led_timer);
    gpio_set_level(CAMERA_USER_LED_GPIO, CAMERA_USER_LED_ACTIVE_LEVEL);
    if (esp_timer_start_once(s_capture_led_timer,
                             CAMERA_USER_LED_BLINK_MS * 1000ULL) != ESP_OK) {
        gpio_set_level(CAMERA_USER_LED_GPIO, !CAMERA_USER_LED_ACTIVE_LEVEL);
    }
}

/*
 * SDMMC is very sensitive to DMA alignment on ESP32-P4.  The standalone
 * high-speed SD test writes from a 64-byte-aligned internal-RAM buffer; writing
 * directly from PSRAM frame buffers can force slow sector-by-sector copies in
 * the driver.  Keep file writes staged through an aligned internal chunk.
 */
static uint8_t *alloc_sd_write_chunk(void)
{
    return heap_caps_aligned_alloc(64, WRITE_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void storage_write_yield(void)
{
    taskYIELD();
}

static esp_err_t fwrite_chunked_dma(FILE *file, const uint8_t *data, size_t size, uint8_t *chunk_buf)
{
    size_t written_total = 0;
    while (written_total < size) {
        size_t n = size - written_total;
        if (n > WRITE_CHUNK_BYTES) {
            n = WRITE_CHUNK_BYTES;
        }

        const uint8_t *src = data + written_total;
        if (chunk_buf) {
            memcpy(chunk_buf, src, n);
            src = chunk_buf;
        }

        size_t written = fwrite(src, 1, n, file);
        written_total += written;
        if (written != n) {
            return ESP_FAIL;
        }
        storage_write_yield();
    }
    return ESP_OK;
}

static camera_timestamp_provider_t s_timestamp_provider;
static camera_orientation_provider_t s_orientation_provider;
static camera_capture_metrics_cb_t s_capture_metrics_cb;
static camera_save_metrics_cb_t s_save_metrics_cb;

static void fourcc_to_str(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xFF);
    out[1] = (char)((fourcc >> 8) & 0xFF);
    out[2] = (char)((fourcc >> 16) & 0xFF);
    out[3] = (char)((fourcc >> 24) & 0xFF);
    out[4] = '\0';
    for (int i = 0; i < 4; ++i) {
        if ((unsigned char)out[i] < 32 || (unsigned char)out[i] > 126) {
            out[i] = '?';
        }
    }
}

static imu_orientation_sample_t capture_orientation(void)
{
    imu_orientation_sample_t sample = {0};
    strlcpy(sample.status, "IMU status unavailable", sizeof(sample.status));
    if (s_orientation_provider != NULL) {
        esp_err_t err = s_orientation_provider(&sample);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Capture-time IMU sample failed: %s", esp_err_to_name(err));
        }
    }
    return sample;
}

/* Thin wrappers keep ioctl setup in one place and make capture sequencing read
 * like a state machine. */
static esp_err_t dequeue_frame(struct v4l2_buffer *buf)
{
    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;

    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)VIDEO_DEQUEUE_TIMEOUT_MS * 1000);
    while (esp_timer_get_time() < deadline_us) {
        errno = 0;
        if (ioctl(s_video_fd, VIDIOC_DQBUF, buf) == 0) {
            return ESP_OK;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed: errno=%d", errno);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "VIDIOC_DQBUF timed out after %d ms", VIDEO_DEQUEUE_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t queue_frame(struct v4l2_buffer *buf)
{
    return ioctl(s_video_fd, VIDIOC_QBUF, buf) == 0 ? ESP_OK : ESP_FAIL;
}

/* After VIDIOC_STREAMOFF the driver owns no queued buffers. Re-queue every MMAP
 * buffer before restarting capture. */
static esp_err_t queue_all_frames(void)
{
    if (s_buffers_queued) {
        return ESP_OK;
    }

    for (uint32_t i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Re-queue video buffer %" PRIu32 " failed: errno=%d", i, errno);
            return ESP_FAIL;
        }
    }
    s_buffers_queued = true;
    return ESP_OK;
}

/* Start/stop the V4L2 capture stream while keeping fd/buffers allocated.
 * Stopping the stream invalidates the "3A settled" assumption because AE/AWB
 * state may restart when frames resume. STREAMOFF also returns ownership of the
 * queued buffers, so the next STREAMON must re-queue them first. */
static esp_err_t stream_set(bool enable)
{
    if (s_video_fd < 0 || s_streaming == enable) {
        return ESP_OK;
    }

    if (enable) {
        ESP_RETURN_ON_ERROR(queue_all_frames(), TAG, "queue frames before stream on");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int request = enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF;
    if (ioctl(s_video_fd, request, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAM%s failed: errno=%d", enable ? "ON" : "OFF", errno);
        return ESP_FAIL;
    }
    s_streaming = enable;
    if (!enable) {
        s_buffers_queued = false;
        s_3a_settled = false;
    }
    return ESP_OK;
}

/* Waveshare exposes brightness as a percent and maps it to the signed V4L2
 * brightness control. This is a post-ISP tone adjustment; it cannot recover
 * highlights that were clipped by sensor exposure/gain. */
static esp_err_t set_isp_brightness(uint32_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (isp_fd < 0) {
        ESP_LOGW(TAG, "open ISP device for brightness failed: errno=%d", errno);
        return ESP_FAIL;
    }

    const int32_t brightness = (int32_t)(0xFF * percent / 100) - 127;
    struct v4l2_ext_control control = {
        .id = V4L2_CID_BRIGHTNESS,
        .value = brightness,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_USER_CLASS,
        .count = 1,
        .controls = &control,
    };

    esp_err_t ret = ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &controls) == 0 ? ESP_OK : ESP_FAIL;
    close(isp_fd);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ISP brightness set to %" PRIu32 "%% (%" PRId32 ")", percent, brightness);
    } else {
        ESP_LOGW(TAG, "set ISP brightness failed: errno=%d", errno);
    }
    return ret;
}

/* Consume frames until the IPA has had enough samples for AE/AWB/ISP state.
 * More frames cost awake time and battery, so this is deliberately a compile-
 * time knob in camera.h rather than an unbounded "wait until perfect". */
static esp_err_t settle_3a(void)
{
    if (s_3a_settled) {
        return ESP_OK;
    }
    const int warmup_frames = CAMERA_STREAM_WARMUP_FRAMES;
    ESP_LOGI(TAG, "esp_video IPA warmup starting: %d frame(s)", warmup_frames);
    for (int i = 0; i < warmup_frames; ++i) {
        struct v4l2_buffer buf;
        ESP_RETURN_ON_ERROR(dequeue_frame(&buf), TAG, "warmup dequeue");
        ESP_RETURN_ON_ERROR(queue_frame(&buf), TAG, "warmup queue");
        if ((i + 1) % 25 == 0 || i + 1 == warmup_frames) {
            ESP_LOGI(TAG, "esp_video IPA warmup progress: %d/%d frame(s)",
                     i + 1,
                     warmup_frames);
        }
    }
    s_3a_settled = true;
    ESP_LOGI(TAG, "esp_video IPA warmup consumed %d frames", warmup_frames);
    return ESP_OK;
}

/* Resume capture after slow writer-side work unless a serial command placed the
 * camera in a paused session. */
static void resume_after_write(void)
{
    /* During a burst, only the capture task may restart CSI. Starting it from
     * the writer while JPEG GDMA is active is unsafe on ESP32-P4 revision 1. */
    if (!s_capture_paused && !s_burst_capture_active) {
        (void)stream_set(true);
    }
}

/* Dedicated storage worker. Encryption and SD writes are deliberately off the
 * capture call path so button/serial commands are not blocked by flash-card
 * latency. */
static void writer_task(void *arg)
{
    (void)arg;
    uint8_t *chunk_buf = alloc_sd_write_chunk();
    watchdog_heartbeat_t heartbeat =
        watchdog_supervisor_register("media_writer", 120000);
    while (true) {
        watchdog_supervisor_beat(heartbeat);
        save_job_t job = {0};
        if (xQueueReceive(s_save_queue, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        s_writer_busy = true;

        int64_t encrypt_start_us = esp_timer_get_time();
        uint8_t plain_digest[32];
        char plain_digest_hex[65];
        size_t digest_len = 0;
        (void)psa_crypto_init();
        bool digest_ok = psa_hash_compute(PSA_ALG_SHA_256, job.data, job.size,
                                          plain_digest, sizeof(plain_digest),
                                          &digest_len) == PSA_SUCCESS &&
                         digest_len == sizeof(plain_digest);
        if (digest_ok) {
            for (size_t i = 0; i < sizeof(plain_digest); ++i) {
                snprintf(&plain_digest_hex[i * 2], 3, "%02x", plain_digest[i]);
            }
            plain_digest_hex[64] = '\0';
        }
        uint8_t *encrypted = NULL;
        size_t encrypted_size = 0;
        const char *media_password = device_credentials_media_password();
        esp_err_t err = media_password
            ? image_crypto_encrypt_blob(job.data, job.size, media_password, &encrypted, &encrypted_size)
            : ESP_ERR_INVALID_STATE;
        int64_t encrypt_done_us = esp_timer_get_time();
        heap_caps_free(job.data);
        if (err != ESP_OK || !encrypted) {
            ESP_LOGE(TAG, "Media encryption failed: %s", esp_err_to_name(err));
            if (s_save_metrics_cb) {
                camera_save_metrics_t metrics = {
                    .encrypt_ms = (uint32_t)((encrypt_done_us - encrypt_start_us) / 1000),
                    .total_ms = (uint32_t)((esp_timer_get_time() - job.capture_start_us) / 1000),
                    .plain_bytes = job.size,
                    .ok = false,
                };
                s_save_metrics_cb(&metrics);
            }
            resume_after_write();
            s_writer_busy = false;
            continue;
        }

        int64_t write_start_us = esp_timer_get_time();
        sd_storage_atomic_file_t atomic_file;
        esp_err_t begin_err = sd_storage_atomic_begin(&atomic_file, job.path, "wb");
        FILE *file = begin_err == ESP_OK ? atomic_file.file : NULL;
        bool write_ok = false;
        if (file) {
            setvbuf(file, NULL, _IONBF, 0);
            write_ok = (fwrite_chunked_dma(file, encrypted, encrypted_size, chunk_buf) == ESP_OK);
            if (write_ok) {
                write_ok = sd_storage_atomic_commit(&atomic_file) == ESP_OK;
            } else {
                sd_storage_atomic_abort(&atomic_file);
            }
        }
        int64_t write_done_us = esp_timer_get_time();
        free(encrypted);

        if (!file || !write_ok) {
            ESP_LOGE(TAG, "Media write failed %zu/%zu: %s",
                     write_ok ? encrypted_size : 0, encrypted_size, job.path);
        } else {
            media_transfer_index_record(
                job.path, job.size, digest_ok ? plain_digest_hex : NULL);
            const char *image_name = strrchr(job.path, '/');
            image_name = image_name ? image_name + 1 : job.path;
            esp_err_t orientation_err = daily_summary_append_image_orientation(
                job.timestamp_valid ? &job.captured_at : NULL,
                image_name, &job.orientation);
            if (orientation_err != ESP_OK) {
                ESP_LOGW(TAG, "Summary orientation append failed for %s", job.path);
            }
            ESP_LOGI(TAG, "Encrypted still capture saved in %" PRId64 " ms (%zu bytes)",
                     (esp_timer_get_time() - job.capture_start_us) / 1000, encrypted_size);
        }
        if (s_save_metrics_cb) {
            camera_save_metrics_t metrics = {
                .encrypt_ms = (uint32_t)((encrypt_done_us - encrypt_start_us) / 1000),
                .write_ms = (uint32_t)((write_done_us - write_start_us) / 1000),
                .total_ms = (uint32_t)((write_done_us - job.capture_start_us) / 1000),
                .plain_bytes = job.size,
                .stored_bytes = write_ok ? encrypted_size : 0,
                .ok = (file && write_ok),
            };
            s_save_metrics_cb(&metrics);
        }
        resume_after_write();
        s_writer_busy = false;
        watchdog_supervisor_beat(heartbeat);
    }
}

esp_err_t camera_init(i2c_master_bus_handle_t shared_i2c_bus)
{
    ESP_RETURN_ON_ERROR(capture_led_init(), TAG, "capture LED init");
    ESP_RETURN_ON_ERROR(scene_classifier_init(&s_scene_classifier), TAG,
                        "scene classifier init");
    /* esp_video owns sensor/ISP setup, but SCCB/I2C is shared with the RTC bus
     * created by main.c. Pass the existing bus so the two devices coordinate. */
    esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = shared_i2c_bus,
            .freq = 100000,
        },
        .reset_pin = CAMERA_PWDN_GPIO,
        .pwdn_pin = CAMERA_PWDN_GPIO,
    };
    esp_video_init_config_t config = {.csi = &csi};
    ESP_RETURN_ON_ERROR(esp_video_init(&config), TAG, "esp_video_init");

    /* Compiled-in drivers probe their module IDs. esp_video selects the
     * detected sensor's default format and matching IPA profile. */
    s_video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK);
    ESP_RETURN_ON_FALSE(s_video_fd >= 0, ESP_FAIL, TAG, "open video device failed");

    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_G_FMT, &format) == 0, ESP_FAIL, TAG, "get format");
#if CAMERA_JPEG_BACKEND_M2M
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
    ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "set UYVY format");
#elif CAMERA_OUTPUT_RGB888
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "set RGB888 format");
#else
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "set RGB565 format");
#endif
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_G_FMT, &format) == 0, ESP_FAIL, TAG, "verify format");
#if CAMERA_JPEG_BACKEND_M2M
    ESP_RETURN_ON_FALSE(format.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY,
                        ESP_ERR_NOT_SUPPORTED, TAG, "driver rejected UYVY");
#elif CAMERA_OUTPUT_RGB888
    ESP_RETURN_ON_FALSE(format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB24,
                        ESP_ERR_NOT_SUPPORTED, TAG, "driver rejected RGB888");
#else
    ESP_RETURN_ON_FALSE(format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565,
                        ESP_ERR_NOT_SUPPORTED, TAG, "driver rejected RGB565");
#endif
    s_width = format.fmt.pix.width;
    s_height = format.fmt.pix.height;
    s_capture_fourcc = format.fmt.pix.pixelformat;

    /* MMAP capture buffers avoid copying raw ISP output through CPU memory.
     * The configured pool supports both still capture and zero-copy bursts. */
    struct v4l2_requestbuffers req = {
        .count = VIDEO_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= VIDEO_BUFFER_COUNT,
                        ESP_FAIL, TAG, "request video buffers");
    for (uint32_t i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_FALSE(ioctl(s_video_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "query buffer");
        s_video_buf_len[i] = buf.length;
        s_video_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                              s_video_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_video_buf[i] != MAP_FAILED, ESP_FAIL, TAG, "mmap buffer");
        ESP_RETURN_ON_ERROR(queue_frame(&buf), TAG, "queue initial buffer");
    }
    s_buffers_queued = true;

    /* JPEG backend:
     * - M2M mode keeps the same V4L2 shape as r4d10n/Espressif examples:
     *   raw ISP frame USERPTR in, one MMAP JPEG buffer out.
     * - The direct backend keeps jpeg_encoder_process() as a build
     *   switch while we validate the prototype. */
#if CAMERA_JPEG_BACKEND_M2M
    ESP_RETURN_ON_ERROR(jpeg_m2m_encoder_open(&s_m2m_jpeg, CAMERA_JPEG_QUALITY),
                        TAG, "V4L2 JPEG M2M open");
    ESP_RETURN_ON_ERROR(jpeg_m2m_encoder_start(&s_m2m_jpeg, s_width, s_height,
                                               format.fmt.pix.pixelformat),
                        TAG, "V4L2 JPEG M2M start");
#else
    /* Hardware JPEG encoder uses a persistent output buffer sized for the
     * largest expected 1920x1080 frame at high quality. */
    jpeg_encode_engine_cfg_t jpeg_cfg = {.intr_priority = 0, .timeout_ms = CAMERA_JPEG_TIMEOUT_MS};
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&jpeg_cfg, &s_jpeg), TAG, "jpeg init");
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    s_jpeg_buf = jpeg_alloc_encoder_mem(JPEG_OUT_BYTES, &mem_cfg, &s_jpeg_buf_size);
    ESP_RETURN_ON_FALSE(s_jpeg_buf, ESP_ERR_NO_MEM, TAG, "jpeg buffer");
#endif

    s_save_queue = xQueueCreate(SAVE_QUEUE_LEN, sizeof(save_job_t));
    s_capture_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_save_queue && s_capture_mutex, ESP_ERR_NO_MEM, TAG, "camera RTOS objects");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(writer_task, "video_writer", 12288, NULL, 2,
                                                &s_writer_task, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "writer task");

    ESP_RETURN_ON_ERROR(stream_set(true), TAG, "start video stream");
    (void)set_isp_brightness(CAMERA_ISP_BRIGHTNESS_PERCENT);
    ESP_LOGI(TAG,
             "esp_video camera ready: %" PRIu32 "x%" PRIu32
             " %s (SC2336/OV5647/Arducam IMX500 auto-detect)",
             s_width,
             s_height,
#if CAMERA_OUTPUT_RGB888
             "RGB888"
#elif CAMERA_JPEG_BACKEND_M2M
             "UYVY->V4L2-M2M-JPEG"
#else
             "RGB565"
#endif
    );
    return ESP_OK;
}

void camera_deinit(void)
{
    if (s_capture_led_timer) {
        (void)esp_timer_stop(s_capture_led_timer);
        (void)esp_timer_delete(s_capture_led_timer);
        s_capture_led_timer = NULL;
    }
    gpio_set_level(CAMERA_USER_LED_GPIO, !CAMERA_USER_LED_ACTIVE_LEVEL);
    (void)stream_set(false);
    if (s_writer_task) {
        vTaskDelete(s_writer_task);
        s_writer_task = NULL;
    }
    if (s_save_queue) {
        vQueueDelete(s_save_queue);
        s_save_queue = NULL;
    }
    if (s_capture_mutex) {
        vSemaphoreDelete(s_capture_mutex);
        s_capture_mutex = NULL;
    }
#if CAMERA_JPEG_BACKEND_M2M
    jpeg_m2m_encoder_close(&s_m2m_jpeg);
#else
    if (s_jpeg) {
        jpeg_del_encoder_engine(s_jpeg);
        s_jpeg = NULL;
    }
    heap_caps_free(s_jpeg_buf);
    s_jpeg_buf = NULL;
#endif
    for (int i = 0; i < VIDEO_BUFFER_COUNT; ++i) {
        if (s_video_buf[i] && s_video_buf[i] != MAP_FAILED) {
            munmap(s_video_buf[i], s_video_buf_len[i]);
        }
        s_video_buf[i] = NULL;
    }
    if (s_video_fd >= 0) {
        close(s_video_fd);
        s_video_fd = -1;
    }
    esp_video_deinit();
}

typedef struct {
    uint32_t buffer_index;
    size_t size;
    int64_t capture_start_us;
    struct tm stamp;
    bool stamp_valid;
    imu_orientation_sample_t orientation;
} burst_raw_frame_t;

static esp_err_t enqueue_encoded_jpeg(const uint8_t *jpeg_data, size_t jpeg_size,
                                      int64_t capture_start_us,
                                      const struct tm *captured_stamp, bool burst,
                                      uint32_t burst_index,
                                      const imu_orientation_sample_t *orientation)
{
    uint8_t *copy = heap_caps_malloc(jpeg_size, MALLOC_CAP_SPIRAM);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, jpeg_data, jpeg_size);

    save_job_t job = {
        .data = copy,
        .size = jpeg_size,
        .capture_start_us = capture_start_us,
    };
    if (orientation != NULL) {
        job.orientation = *orientation;
    } else {
        strlcpy(job.orientation.status, "IMU status unavailable",
                sizeof(job.orientation.status));
    }
    if (captured_stamp) {
        job.captured_at = *captured_stamp;
        job.timestamp_valid = true;
        snprintf(job.path, sizeof(job.path),
                 burst
                     ? SD_CAPTURE_DIR "/%04dy%02dm%02dd_%02dh%02dmin%02ds_burst_%02" PRIu32 ".jpg"
                     : SD_CAPTURE_DIR "/%04dy%02dm%02dd_%02dh%02dmin%02ds_%06" PRIu32 ".jpg",
                 captured_stamp->tm_year + 1900, captured_stamp->tm_mon + 1,
                 captured_stamp->tm_mday, captured_stamp->tm_hour,
                 captured_stamp->tm_min, captured_stamp->tm_sec,
                 burst ? burst_index : s_sequence);
    } else {
        snprintf(job.path, sizeof(job.path),
                 burst ? SD_CAPTURE_DIR "/cap_burst_%02" PRIu32 ".jpg"
                       : SD_CAPTURE_DIR "/cap_%06" PRIu32 ".jpg",
                 burst ? burst_index : s_sequence);
    }
    if (xQueueSend(s_save_queue, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        heap_caps_free(copy);
        return ESP_ERR_TIMEOUT;
    }
    ++s_sequence;
    return ESP_OK;
}

esp_err_t camera_capture_once(void)
{
    if (s_capture_paused || !s_capture_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_capture_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    int64_t started = esp_timer_get_time();

    /* The capture lock serializes manual snaps, auto snaps, and transfer pauses.
     * Keep the critical path explicit: settle, grab one frame, stop stream,
     * encode, then hand off storage. */
    esp_err_t err = stream_set(true);
    if (err == ESP_OK) err = settle_3a();

    struct v4l2_buffer frame;
    if (err == ESP_OK) err = dequeue_frame(&frame);
    struct tm captured_stamp = {0};
    bool stamp_valid = err == ESP_OK && s_timestamp_provider &&
                       s_timestamp_provider(&captured_stamp) == ESP_OK;
    imu_orientation_sample_t orientation = capture_orientation();
    if (err == ESP_OK) capture_led_blink();
    if (err == ESP_OK) {
        /* CSI/ISP DMA wrote this buffer. Invalidate the CPU cache before any
         * CPU-side classification or copy, otherwise stale cache lines appear
         * as regular vertical stripes. Classification borrows this buffer and
         * never retains ownership. */
        esp_cache_msync(s_video_buf[frame.index], frame.bytesused,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (s_capture_fourcc == V4L2_PIX_FMT_UYVY) {
            scene_classifier_update_from_uyvy(&s_scene_classifier,
                                              s_video_buf[frame.index], frame.bytesused,
                                              &s_scene_stats);
        } else if (s_capture_fourcc == V4L2_PIX_FMT_RGB565) {
            scene_classifier_update_from_rgb565(&s_scene_classifier,
                                                s_video_buf[frame.index], frame.bytesused,
                                                &s_scene_stats);
        }
    }
    /* Rev-1 P4 cannot reliably run CSI/ISP and JPEG GDMA concurrently. Keep
     * the completed MMAP frame, but stop capture before JPEG reads it. */
    if (err == ESP_OK) err = stream_set(false);
    uint32_t jpeg_size = 0;
    uint8_t *jpeg_data = NULL;
#if CAMERA_JPEG_BACKEND_M2M
    bool release_m2m_jpeg = false;
#endif
    if (err == ESP_OK) {
#if CAMERA_JPEG_BACKEND_M2M
        err = jpeg_m2m_encoder_encode(&s_m2m_jpeg, s_video_buf[frame.index], frame.bytesused,
                                      &jpeg_data, &jpeg_size);
        if (err == ESP_OK) {
            release_m2m_jpeg = true;
        }
#else
        /* The completed frame came from CSI/ISP DMA. Refresh the CPU's cached
         * view before classification and before copying it into burst PSRAM. */
        esp_cache_msync(s_video_buf[frame.index], frame.bytesused,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        jpeg_encode_cfg_t enc = {
#if CAMERA_OUTPUT_RGB888
            .src_type = JPEG_ENC_SRC_RGB888,
#else
            .src_type = JPEG_ENC_SRC_RGB565,
#endif
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = CAMERA_JPEG_QUALITY,
            .width = s_width,
            .height = s_height,
        };
        err = jpeg_encoder_process(s_jpeg, &enc, s_video_buf[frame.index], frame.bytesused,
                                   s_jpeg_buf, s_jpeg_buf_size, &jpeg_size);
        jpeg_data = s_jpeg_buf;
#endif
        esp_err_t qerr = queue_all_frames();
        if (err == ESP_OK) err = qerr;
    }

    if (err == ESP_OK) {
        err = enqueue_encoded_jpeg(jpeg_data, jpeg_size, started,
                                   stamp_valid ? &captured_stamp : NULL,
                                   s_burst_capture_active, 0, &orientation);
    }
#if CAMERA_JPEG_BACKEND_M2M
    if (release_m2m_jpeg) {
        esp_err_t rerr = jpeg_m2m_encoder_release(&s_m2m_jpeg);
        if (err == ESP_OK) {
            err = rerr;
        }
    }
#endif
    if (err != ESP_OK) resume_after_write();
    if (s_capture_metrics_cb) {
        s_capture_metrics_cb((uint32_t)((esp_timer_get_time() - started) / 1000), err);
    }
    xSemaphoreGive(s_capture_mutex);
    return err;
}

esp_err_t camera_capture_burst(uint32_t duration_ms, uint32_t *captured_count)
{
    (void)duration_ms;
    if (captured_count) *captured_count = 0;
    if (s_capture_paused || !s_capture_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_capture_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int64_t started_us = esp_timer_get_time();
    burst_raw_frame_t raw[CAMERA_BURST_IMAGE_COUNT] = {0};
    uint32_t count = 0;
    esp_err_t err = stream_set(true);
    s_burst_capture_active = true;

    /* Burst phase 1 - acquisition. Keep CSI/ISP running for the whole phase.
     * One full warmup
     * establishes AE/AWB, then consecutive frames are copied to PSRAM without
     * resetting the ISP between exposures. */
    if (err == ESP_OK) err = settle_3a();
    while (err == ESP_OK && count < CAMERA_BURST_IMAGE_COUNT) {
        struct v4l2_buffer frame;
        err = dequeue_frame(&frame);
        if (err != ESP_OK) break;

        burst_raw_frame_t *captured = &raw[count];
        captured->capture_start_us = esp_timer_get_time();
        captured->stamp_valid = s_timestamp_provider &&
                                s_timestamp_provider(&captured->stamp) == ESP_OK;
        captured->orientation = capture_orientation();
        captured->buffer_index = frame.index;
        captured->size = frame.bytesused;
        /* Keep ownership of each completed MMAP buffer until the entire burst
         * is acquired. This avoids copying full frames through PSRAM while CSI
         * is active and gives JPEG the same DMA-backed input used by reliable
         * single captures. */
        esp_cache_msync(s_video_buf[frame.index], frame.bytesused,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (s_capture_fourcc == V4L2_PIX_FMT_UYVY) {
            scene_classifier_update_from_uyvy(&s_scene_classifier,
                                              s_video_buf[frame.index], frame.bytesused,
                                              &s_scene_stats);
        } else if (s_capture_fourcc == V4L2_PIX_FMT_RGB565) {
            scene_classifier_update_from_rgb565(&s_scene_classifier,
                                                s_video_buf[frame.index], frame.bytesused,
                                                &s_scene_stats);
        }
        capture_led_blink();
        ++count;
    }

    /* Burst phase 2 - encoding. Revision-1 P4 cannot run CSI/ISP and JPEG GDMA
     * concurrently. All raw exposures are held now, so stop CSI before JPEG
     * reads those DMA-backed buffers. */
    if (s_streaming) {
        esp_err_t stop_err = stream_set(false);
        if (err == ESP_OK) err = stop_err;
    }

    uint32_t encoded = 0;
    for (uint32_t i = 0; err == ESP_OK && i < count; ++i) {
        uint8_t *jpeg_data = NULL;
        uint32_t jpeg_size = 0;
#if CAMERA_JPEG_BACKEND_M2M
        bool release_m2m_jpeg = false;
        err = jpeg_m2m_encoder_encode(&s_m2m_jpeg,
                                      s_video_buf[raw[i].buffer_index], raw[i].size,
                                      &jpeg_data, &jpeg_size);
        if (err == ESP_OK) release_m2m_jpeg = true;
#else
        esp_cache_msync(s_video_buf[raw[i].buffer_index], raw[i].size,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        jpeg_encode_cfg_t enc = {
#if CAMERA_OUTPUT_RGB888
            .src_type = JPEG_ENC_SRC_RGB888,
#else
            .src_type = JPEG_ENC_SRC_RGB565,
#endif
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = CAMERA_JPEG_QUALITY,
            .width = s_width,
            .height = s_height,
        };
        err = jpeg_encoder_process(s_jpeg, &enc,
                                   s_video_buf[raw[i].buffer_index], raw[i].size,
                                   s_jpeg_buf, s_jpeg_buf_size, &jpeg_size);
        jpeg_data = s_jpeg_buf;
#endif
        if (err == ESP_OK) {
            err = enqueue_encoded_jpeg(jpeg_data, jpeg_size, raw[i].capture_start_us,
                                       raw[i].stamp_valid ? &raw[i].stamp : NULL,
                                       true, i + 1U, &raw[i].orientation);
            if (err == ESP_OK) ++encoded;
        }
#if CAMERA_JPEG_BACKEND_M2M
        if (release_m2m_jpeg) {
            esp_err_t release_err = jpeg_m2m_encoder_release(&s_m2m_jpeg);
            if (err == ESP_OK) err = release_err;
        }
#endif
    }

    /* Burst phase 3 - persistence. The writer owns each queued JPEG copy;
     * waiting here makes a successful burst mean every image reached the SD
     * card before capture resumes. */
    if (err == ESP_OK) err = camera_wait_for_idle(30000);
    s_burst_capture_active = false;
    if (!s_capture_paused) {
        esp_err_t resume_err = stream_set(true);
        if (err == ESP_OK) err = resume_err;
    }
    if (captured_count) *captured_count = encoded;
    if (s_capture_metrics_cb) {
        s_capture_metrics_cb((uint32_t)((esp_timer_get_time() - started_us) / 1000), err);
    }
    xSemaphoreGive(s_capture_mutex);

    ESP_LOGI(TAG,
             "PSRAM burst complete: %" PRIu32 " exposure(s), %" PRIu32
             " saved in %" PRIu32 " ms",
             count, encoded, (uint32_t)((esp_timer_get_time() - started_us) / 1000));
    return err;
}

esp_err_t camera_wait_for_idle(uint32_t timeout_ms)
{
    /* Used before deep sleep and serial media transfer: both need a quiet SD
     * card and no pending encryption/write job. */
    TickType_t start = xTaskGetTickCount();
    while (s_writer_busy || (s_save_queue && uxQueueMessagesWaiting(s_save_queue))) {
        if (timeout_ms && xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

void camera_set_capture_paused(bool paused) { s_capture_paused = paused; }
esp_err_t camera_suspend_stream(void) { return stream_set(false); }
esp_err_t camera_resume_stream(void) { return s_capture_paused ? ESP_OK : stream_set(true); }

esp_err_t camera_print_v4l2_formats(void)
{
    if (s_video_fd < 0) {
        printf("ERR V4L2_FORMATS camera_not_ready\n");
        fflush(stdout);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_capture_mutex && xSemaphoreTake(s_capture_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        printf("ERR V4L2_FORMATS busy\n");
        fflush(stdout);
        return ESP_ERR_TIMEOUT;
    }

    struct v4l2_format format = {0};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_G_FMT, &format) != 0) {
        if (s_capture_mutex) {
            xSemaphoreGive(s_capture_mutex);
        }
        printf("ERR V4L2_FORMATS get_format errno=%d\n", errno);
        fflush(stdout);
        return ESP_FAIL;
    }

    char fourcc[5];
    fourcc_to_str(format.fmt.pix.pixelformat, fourcc);
    printf("FMT_CURRENT %" PRIu32 "x%" PRIu32 " 0x%08" PRIX32 " %s\n",
           format.fmt.pix.width,
           format.fmt.pix.height,
           format.fmt.pix.pixelformat,
           fourcc);

    for (uint32_t index = 0;; ++index) {
        struct v4l2_fmtdesc fmtdesc = {
            .index = index,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };

        if (ioctl(s_video_fd, VIDIOC_ENUM_FMT, &fmtdesc) != 0) {
            break;
        }

        fourcc_to_str(fmtdesc.pixelformat, fourcc);
        printf("FMT %" PRIu32 " %s 0x%08" PRIX32 " %s\n",
               index,
               (char *)fmtdesc.description,
               fmtdesc.pixelformat,
               fourcc);
    }

    if (s_capture_mutex) {
        xSemaphoreGive(s_capture_mutex);
    }
    printf("OK V4L2_FORMATS\n");
    fflush(stdout);
    return ESP_OK;
}

void camera_set_timestamp_provider(camera_timestamp_provider_t provider) { s_timestamp_provider = provider; }
void camera_set_orientation_provider(camera_orientation_provider_t provider) { s_orientation_provider = provider; }

void camera_set_metrics_callbacks(camera_capture_metrics_cb_t capture_cb,
                                  camera_save_metrics_cb_t save_cb)
{
    s_capture_metrics_cb = capture_cb;
    s_save_metrics_cb = save_cb;
}

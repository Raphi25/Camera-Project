/* Media service API and injected camera/transport operations. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * SD media inventory and host-transfer service.
 *
 * The module owns filename validation, encrypted/plain media loading, transfer
 * framing, CRC generation, and deletion. Camera coordination is injected so
 * this service does not depend on scheduler globals.
 */
typedef struct {
    void (*set_capture_paused)(bool paused);
    esp_err_t (*wait_for_camera_idle)(uint32_t timeout_ms);
    esp_err_t (*suspend_camera_stream)(void);
    esp_err_t (*resume_camera_stream)(void);
    bool (*scheduler_is_paused)(void);
    void (*notify_transfer_state)(bool active);
} media_transfer_camera_ops_t;

typedef void (*media_transfer_write_cb_t)(void *ctx, const char *data, size_t len);

typedef struct {
    media_transfer_write_cb_t write;
    void *ctx;
} media_transfer_output_t;

esp_err_t media_transfer_init(const media_transfer_camera_ops_t *camera_ops);
bool media_transfer_is_active(void);
int media_transfer_count(void);
void media_transfer_index_record(const char *full_path, size_t plain_size,
                                 const char *sha256_hex);

void media_transfer_command_count(const media_transfer_output_t *output);
void media_transfer_command_list(const media_transfer_output_t *output);
void media_transfer_command_delete_all(const media_transfer_output_t *output);
void media_transfer_command_get(const char *file_name, size_t offset, bool use_base64,
                                bool use_binary,
                                const media_transfer_output_t *output);
void media_transfer_command_pause(const media_transfer_output_t *output);
void media_transfer_command_resume(const media_transfer_output_t *output);

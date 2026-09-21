/* Pause-safe, resumable, integrity-checked transfer of captures from SD. */

#include "media_transfer.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "camera.h"
#include "device_settings.h"
#include "device_credentials.h"
#include "image_crypto.h"
#include "sd_storage.h"
#include "protocol_frame.h"

static media_transfer_camera_ops_t s_camera_ops;
static bool s_initialized;
static volatile bool s_transfer_active;
static bool require_sd(const char *command, const media_transfer_output_t *output);

#define MEDIA_INDEX_PATH SD_CAPTURE_DIR "/.media_index_v1"
#define MEDIA_INDEX_TEMP_PATH SD_CAPTURE_DIR "/.media_index_v1.tmp"

static bool output_write(const media_transfer_output_t *output, const char *data, size_t len)
{
    if (output == NULL || output->write == NULL || data == NULL) {
        return false;
    }
    output->write(output->ctx, data, len);
    return true;
}

static void output_printf(const media_transfer_output_t *output, const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len <= 0) return;
    size_t out_len = (size_t)len < sizeof(buffer) ? (size_t)len : sizeof(buffer) - 1;
    (void)output_write(output, buffer, out_len);
}

esp_err_t media_transfer_init(const media_transfer_camera_ops_t *camera_ops)
{
    if (camera_ops == NULL || camera_ops->set_capture_paused == NULL ||
        camera_ops->wait_for_camera_idle == NULL ||
        camera_ops->suspend_camera_stream == NULL ||
        camera_ops->resume_camera_stream == NULL ||
        camera_ops->scheduler_is_paused == NULL ||
        camera_ops->notify_transfer_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_camera_ops = *camera_ops;
    s_transfer_active = false;
    s_initialized = true;
    return ESP_OK;
}

bool media_transfer_is_active(void)
{
    return s_transfer_active;
}

void media_transfer_command_pause(const media_transfer_output_t *output)
{
    if (!s_initialized) {
        output_printf(output, "ERR IMG_PAUSE not_initialized\n");
return;
    }
    s_camera_ops.set_capture_paused(true);
    esp_err_t idle_err = s_camera_ops.wait_for_camera_idle(5000);
    esp_err_t suspend_err = idle_err == ESP_OK
        ? s_camera_ops.suspend_camera_stream()
        : idle_err;
    if (suspend_err == ESP_OK) {
        output_printf(output, "OK IMG_PAUSE\n");
    } else {
        output_printf(output, "ERR IMG_PAUSE %s\n", esp_err_to_name(suspend_err));
    }
}

void media_transfer_command_resume(const media_transfer_output_t *output)
{
    if (!s_initialized) {
        output_printf(output, "ERR IMG_RESUME not_initialized\n");
return;
    }
    esp_err_t err = s_camera_ops.resume_camera_stream();
    if (err == ESP_OK) {
        s_camera_ops.set_capture_paused(false);
        output_printf(output, "OK IMG_RESUME\n");
    } else {
        output_printf(output, "ERR IMG_RESUME %s\n", esp_err_to_name(err));
    }
}

static bool has_media_extension(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot == NULL) {
        return false;
    }
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".mjpeg") == 0 ||
           strcasecmp(dot, ".mjpg") == 0;
}

static bool is_orientation_stage_name(const char *name)
{
    if (name == NULL || strncmp(name, "summary_", 8) != 0) return false;
    const char *ext = strrchr(name, '.');
    return ext != NULL && strcasecmp(ext, ".orientation") == 0;
}

static bool is_debug_snapshot(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot == NULL || strcasecmp(dot, ".jpg") != 0 || (size_t)(dot - name) < 4U) {
        return false;
    }
    const char *suffix = dot - 4;
    return suffix[0] == '_' && tolower((unsigned char)suffix[1]) == 'f' &&
           isdigit((unsigned char)suffix[2]) && isdigit((unsigned char)suffix[3]);
}

static bool is_safe_name(const char *name)
{
    if (name == NULL || *name == '\0' || !has_media_extension(name)) return false;
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') return false;
    }
    return true;
}

void media_transfer_index_record(const char *full_path, size_t plain_size,
                                 const char *sha256_hex)
{
    /* The index turns IMG_LIST from many slow random SD reads into one
     * sequential read. Only committed captures reach this function. */
    if (full_path == NULL || plain_size == 0 || !sd_storage_is_mounted()) return;
    const char *name = strrchr(full_path, '/');
    name = name ? name + 1 : full_path;
    if (!is_safe_name(name) || is_debug_snapshot(name)) return;

    sd_storage_lock();
    FILE *probe = fopen(MEDIA_INDEX_PATH, "rb");
    if (probe == NULL) {
        /* Create the index immediately only when this is the first media file.
         * Populated cards still use IMG_LIST's complete migration rebuild. */
        int media_count = 0;
        DIR *dir = opendir(SD_CAPTURE_DIR);
        if (dir != NULL) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] != '.' && has_media_extension(entry->d_name) &&
                    !is_debug_snapshot(entry->d_name)) {
                    media_count++;
                }
            }
            closedir(dir);
        }
        if (media_count != 1) {
            sd_storage_unlock();
            return;
        }
    } else {
        fclose(probe);
    }
    FILE *index = fopen(MEDIA_INDEX_PATH, probe == NULL ? "wb" : "ab");
    if (index != NULL) {
        if (sha256_hex != NULL && strlen(sha256_hex) == 64U) {
            fprintf(index, "%s\t%u\t%s\n", name, (unsigned)plain_size, sha256_hex);
        } else {
            fprintf(index, "%s\t%u\n", name, (unsigned)plain_size);
        }
        fflush(index);
        fclose(index);
    }
    sd_storage_unlock();
}

static int emit_media_index(FILE *index, const media_transfer_output_t *output)
{
    char line[384];
    int count = 0;
    while (fgets(line, sizeof(line), index) != NULL) {
        char name[256];
        unsigned size = 0;
        if (sscanf(line, "%255s %u", name, &size) != 2) continue;
        if (size > 0 && is_safe_name(name) && !is_debug_snapshot(name)) {
            output_printf(output, "IMG %s %u\n", name, size);
            count++;
        }
    }
    return count;
}

static bool media_index_find_hash(const char *file_name, char out_hash[65])
{
    FILE *index = fopen(MEDIA_INDEX_PATH, "rb");
    if (index == NULL) return false;
    char line[384];
    bool found = false;
    while (fgets(line, sizeof(line), index) != NULL) {
        char name[256];
        char hash[65];
        unsigned size = 0;
        if (sscanf(line, "%255s %u %64s", name, &size, hash) == 3 &&
            strcmp(name, file_name) == 0 && strlen(hash) == 64U) {
            memcpy(out_hash, hash, 65U);
            found = true;
            break;
        }
    }
    fclose(index);
    return found;
}

static esp_err_t send_payload(const char *name, const uint8_t *data, size_t len,
                              size_t start_offset, bool b64, bool binary,
                              const char *known_digest_hex,
                              const media_transfer_output_t *output)
{
    /* BEGIN/END stay textual for recovery and diagnostics. Payload frames use
     * the negotiated binary, Base64, or legacy hex encoding. */
    const size_t chunk_size = binary
        ? DEVICE_IMAGE_BINARY_CHUNK_BYTES
        : DEVICE_IMAGE_TX_CHUNK_BYTES;
    const size_t encoded_cap = DEVICE_IMAGE_TX_CHUNK_BYTES * 2 + 1;
    char *encoded = binary ? NULL : malloc(encoded_cap);
    char *line = binary
        ? malloc(DEVICE_IMAGE_BINARY_CHUNK_BYTES + 12U)
        : malloc(encoded_cap + 64U);
    if ((!binary && !encoded) || !line) {
        free(encoded); free(line); return ESP_ERR_NO_MEM;
    }
    uint8_t digest[32];
    char digest_hex[65];
    /* CRC protects each transport frame and permits precise chunk rejection;
     * SHA-256 covers the complete plaintext and catches missing, duplicated,
     * or reordered data after the receiver reconstructs the file. */
    if (known_digest_hex != NULL && strlen(known_digest_hex) == 64U) {
        memcpy(digest_hex, known_digest_hex, sizeof(digest_hex));
    } else {
        size_t digest_len = 0;
        (void)psa_crypto_init();
        if (psa_hash_compute(PSA_ALG_SHA_256, data, len, digest, sizeof(digest),
                             &digest_len) != PSA_SUCCESS || digest_len != sizeof(digest)) {
            free(encoded); free(line); return ESP_FAIL;
        }
        for (size_t i = 0; i < sizeof(digest); ++i) {
            snprintf(&digest_hex[i * 2], 3, "%02x", digest[i]);
        }
        digest_hex[64] = '\0';
    }
    output_printf(output, "BEGIN %s %u %u %s\n", name, (unsigned)len,
                  (unsigned)start_offset, digest_hex);
    /* start_offset supports resumable downloads. Sequence numbers restart at
     * zero for each response, while BEGIN carries the absolute file offset. */
    for (size_t offset = start_offset, seq = 0; offset < len; seq++) {
        size_t n = len - offset > chunk_size ? chunk_size : len - offset;
        if (binary) {
            uint16_t crc = protocol_frame_crc16_ccitt(data + offset, n);
            uint8_t *frame = (uint8_t *)line;
            const uint8_t header[12] = {
                'R', 'I', 'M', 'G',
                (uint8_t)seq, (uint8_t)(seq >> 8),
                (uint8_t)(seq >> 16), (uint8_t)(seq >> 24),
                (uint8_t)n, (uint8_t)(n >> 8),
                (uint8_t)crc, (uint8_t)(crc >> 8),
            };
            memcpy(frame, header, sizeof(header));
            memcpy(frame + sizeof(header), data + offset, n);
            if (!output_write(output, line, sizeof(header) + n)) {
                free(encoded); free(line); return ESP_FAIL;
            }
            offset += n;
            if ((seq & 15U) == 15U) taskYIELD();
            continue;
        }
        if (b64) {
            size_t out_len = 0;
            if (mbedtls_base64_encode((unsigned char *)encoded, encoded_cap, &out_len,
                                      data + offset, n) != 0) {
                free(encoded); free(line); return ESP_FAIL;
            }
            encoded[out_len] = '\0';
        } else {
            size_t line_len = protocol_frame_encode_hex(
                (uint32_t)seq, data + offset, n, line, encoded_cap + 64);
            if (line_len == 0 || !output_write(output, line, line_len)) {
                free(encoded); free(line); return ESP_FAIL;
            }
            offset += n;
            if ((seq & 15U) == 15U) taskYIELD();
            continue;
        }
        int line_len = snprintf(line, encoded_cap + 64,
            "DATA64 %u %u %04X %s\n",
            (unsigned)seq, (unsigned)n,
            (unsigned)protocol_frame_crc16_ccitt(data + offset, n), encoded);
        if (line_len <= 0 || !output_write(output, line, (size_t)line_len)) {
            free(encoded); free(line); return ESP_FAIL;
        }
        offset += n;
        if ((seq & 15U) == 15U) taskYIELD();
    }
    output_printf(output, "END %s %s\n", name, digest_hex);
    free(encoded);
    free(line);
    return ESP_OK;
}

void media_transfer_command_get(const char *file_name, size_t offset, bool use_base64,
                                bool use_binary,
                                const media_transfer_output_t *output)
{
    /* A transfer owns the SD card and pauses capture so encrypted-file reads,
     * USB output, and camera DMA cannot contend for the same resources. */
    if (!s_initialized || !is_safe_name(file_name)) {
        output_printf(output, "ERR IMG_GET invalid_name\n"); return;
    }
    if (!require_sd("IMG_GET", output)) return;
    char path[320];
    if (snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", file_name) >= (int)sizeof(path)) {
        output_printf(output, "ERR IMG_GET name_too_long\n"); return;
    }
    bool was_paused = s_camera_ops.scheduler_is_paused();
    s_transfer_active = true;
    s_camera_ops.notify_transfer_state(true);
    s_camera_ops.set_capture_paused(true);
    (void)s_camera_ops.wait_for_camera_idle(3000);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_log_level_t old_level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    sd_storage_lock();
    const char *password = device_credentials_media_password();
    esp_err_t err = password
        ? image_crypto_load_plain_from_file(path, password, &plain, &plain_len)
        : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            rewind(f);
            if (size > 0) {
                plain = malloc((size_t)size);
                if (plain && fread(plain, 1, (size_t)size, f) == (size_t)size) {
                    plain_len = (size_t)size;
                    err = ESP_OK;
                } else {
                    free(plain);
                    plain = NULL;
                    err = ESP_FAIL;
                }
            }
            fclose(f);
        }
    }
    if (err == ESP_OK && offset > plain_len) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && plain && plain_len) {
        char indexed_hash[65];
        const char *known_hash = media_index_find_hash(file_name, indexed_hash)
            ? indexed_hash
            : NULL;
        if (use_binary) {
            /* stdout normally expands LF to CRLF. That corrupts arbitrary
             * binary headers and payload bytes, so make this transfer raw. */
            usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
        }
        err = send_payload(file_name, plain, plain_len, offset, use_base64,
                           use_binary, known_hash, output);
        if (use_binary) {
            usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
        }
    }
    free(plain);
    sd_storage_unlock();
    esp_log_level_set("*", old_level);
    s_camera_ops.set_capture_paused(was_paused);
    s_transfer_active = false;
    s_camera_ops.notify_transfer_state(false);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            output_printf(output, "ERR IMG_GET not_found\n");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            output_printf(output, "ERR IMG_GET invalid_offset\n");
        } else {
            output_printf(output, "ERR IMG_GET tx_write\n");
        }
    }
}

static bool require_sd(const char *command, const media_transfer_output_t *output)
{
    esp_err_t err = sd_storage_require_mounted();
    if (err == ESP_OK) {
        return true;
    }
    output_printf(output, "ERR %s SD_NOT_READY %s\n", command, esp_err_to_name(err));
    return false;
}

int media_transfer_count(void)
{
    if (!sd_storage_is_mounted()) {
        return -1;
    }
    sd_storage_lock();
    DIR *dir = opendir(SD_CAPTURE_DIR);
    if (dir == NULL) {
        sd_storage_unlock();
        return -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.' && has_media_extension(entry->d_name) &&
            !is_debug_snapshot(entry->d_name)) {
            count++;
        }
    }
    closedir(dir);
    sd_storage_unlock();
    return count;
}

void media_transfer_command_count(const media_transfer_output_t *output)
{
    if (!require_sd("IMG_COUNT", output)) {
        return;
    }
    int count = media_transfer_count();
    output_printf(output, count < 0 ? "ERR IMG_COUNT open_dir_failed\n" : "COUNT %d\n", count);
}

void media_transfer_command_list(const media_transfer_output_t *output)
{
    /* Normal operation reads the persistent index. Directory scanning is a
     * one-time migration path for cards created by older firmware. */
    if (!require_sd("IMG_LIST", output)) {
        return;
    }
    sd_storage_lock();
    FILE *index = fopen(MEDIA_INDEX_PATH, "rb");
    if (index != NULL) {
        (void)emit_media_index(index, output);
        fclose(index);
        sd_storage_unlock();
        output_printf(output, "OK LIST\n");
        return;
    }

    /* One-time migration path for cards populated before the index existed. */
    DIR *dir = opendir(SD_CAPTURE_DIR);
    if (dir == NULL) {
        sd_storage_unlock();
        output_printf(output, "ERR IMG_LIST open_dir_failed\n");
        return;
    }
    FILE *new_index = fopen(MEDIA_INDEX_TEMP_PATH, "wb");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || !has_media_extension(entry->d_name) ||
            is_debug_snapshot(entry->d_name)) {
            continue;
        }
        char path[320];
        if (snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        size_t plain_size = 0;
        struct stat st = {0};
        const char *password = device_credentials_media_password();
        esp_err_t err = password
            ? image_crypto_get_plain_size_from_file(path, password, &plain_size)
            : ESP_ERR_INVALID_STATE;
        if ((err != ESP_OK || plain_size == 0) &&
            (stat(path, &st) != 0 || st.st_size <= 0)) {
            continue;
        }
        if (plain_size == 0) {
            plain_size = (size_t)st.st_size;
        }
        output_printf(output, "IMG %s %u\n", entry->d_name, (unsigned)plain_size);
        if (new_index != NULL) {
            fprintf(new_index, "%s\t%u\n", entry->d_name, (unsigned)plain_size);
        }
    }
    closedir(dir);
    if (new_index != NULL) {
        fflush(new_index);
        fclose(new_index);
        (void)unlink(MEDIA_INDEX_PATH);
        if (rename(MEDIA_INDEX_TEMP_PATH, MEDIA_INDEX_PATH) != 0) {
            (void)unlink(MEDIA_INDEX_TEMP_PATH);
        }
    }
    sd_storage_unlock();
    output_printf(output, "OK LIST\n");
}

void media_transfer_command_delete_all(const media_transfer_output_t *output)
{
    if (!require_sd("IMG_DELETE_ALL", output)) {
        return;
    }
    sd_storage_lock();
    DIR *dir = opendir(SD_CAPTURE_DIR);
    if (dir == NULL) {
        sd_storage_unlock();
        output_printf(output, "ERR IMG_DELETE_ALL open_dir_failed\n");
return;
    }
    int deleted = 0;
    int failed = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' ||
            (!has_media_extension(entry->d_name) &&
             !is_orientation_stage_name(entry->d_name))) {
            continue;
        }
        char path[320];
        if (snprintf(path, sizeof(path), SD_CAPTURE_DIR "/%s", entry->d_name) >= (int)sizeof(path)) {
            failed++;
        } else if (unlink(path) == 0) {
            deleted++;
        } else {
            failed++;
        }
    }
    closedir(dir);
    (void)unlink(MEDIA_INDEX_PATH);
    (void)unlink(MEDIA_INDEX_TEMP_PATH);
    sd_storage_unlock();
    if (failed) {
        output_printf(output, "ERR IMG_DELETE_ALL partial deleted=%d failed=%d\n", deleted, failed);
    } else {
        output_printf(output, "OK IMG_DELETE_ALL deleted=%d\n", deleted);
    }
}

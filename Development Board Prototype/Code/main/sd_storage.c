/* Board-specific SDMMC mount, serialization, atomic writes, and recovery. */

#include "sd_storage.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "event_breadcrumbs.h"

#define SDMMC_PIN_CLK GPIO_NUM_43
#define SDMMC_PIN_CMD GPIO_NUM_44
#define SDMMC_PIN_D0  GPIO_NUM_39
#define SDMMC_PIN_D1  GPIO_NUM_40
#define SDMMC_PIN_D2  GPIO_NUM_41
#define SDMMC_PIN_D3  GPIO_NUM_42
#define SD_PWRN_GPIO  GPIO_NUM_45
#define SD_PWRN_ON_LEVEL  0
#define SD_PWRN_OFF_LEVEL 1
#define SD_BENCH_PATH SD_CAPTURE_DIR "/sd_bench.tmp"

static const char *TAG = "sd_storage";
static SemaphoreHandle_t s_sd_mutex = NULL;
static bool s_sd_mounted = false;
static bool s_usb_owned = false;
static sdmmc_card_t *s_card = NULL;
static esp_err_t s_sd_last_err = ESP_ERR_INVALID_STATE;
static bool s_atomic_recovery_done = false;

/* ESP-Hosted on IDF 6 initializes the shared SDMMC controller before
 * app_main(). Slot 1 belongs to the C6 transport and slot 0 belongs to the SD
 * card, but the VFS convenience mount must not initialize/deinitialize the
 * common controller a second time. This matches Espressif's
 * host_sdcard_with_hosted example. */
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
static esp_err_t shared_sdmmc_host_init(void)
{
    return ESP_OK;
}

static esp_err_t shared_sdmmc_host_deinit(void)
{
    return ESP_OK;
}
#endif

typedef struct {
    int width;
    int max_freq_khz;
    const char *label;
} sd_mount_profile_t;

static const sd_mount_profile_t SD_MOUNT_PROFILES[] = {
    {4, SDMMC_FREQ_HIGHSPEED, "SDMMC 4-bit high-speed"},
    {1, SDMMC_FREQ_HIGHSPEED, "SDMMC 1-bit high-speed"},
    {1, SDMMC_FREQ_DEFAULT, "SDMMC 1-bit default-speed"},
    {1, SDMMC_FREQ_PROBING, "SDMMC 1-bit probing-speed"},
};

static esp_err_t sd_storage_set_card_power(bool on)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << SD_PWRN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "Failed to configure SD power gate GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(SD_PWRN_GPIO, on ? SD_PWRN_ON_LEVEL : SD_PWRN_OFF_LEVEL),
                        TAG, "Failed to drive SD power gate GPIO");

    ESP_LOGI(TAG, "SD card MOSFET power gate GPIO%d -> %s", SD_PWRN_GPIO, on ? "ON" : "OFF");
    vTaskDelay(pdMS_TO_TICKS(on ? 20 : 5));
    return ESP_OK;
}

esp_err_t sd_storage_mount(sdmmc_card_t **out_card)
{
    if (out_card != NULL) {
        *out_card = NULL;
    }

    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (s_sd_mutex == NULL) {
            s_sd_mounted = false;
            s_sd_last_err = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_RETURN_ON_ERROR(sd_storage_set_card_power(true), TAG, "Failed to enable SD card power gate");

    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl = NULL;
    esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr_ctrl);
    if (pwr_err != ESP_OK) {
        (void)sd_storage_set_card_power(false);
        s_sd_mounted = false;
        s_sd_last_err = pwr_err;
        ESP_RETURN_ON_ERROR(pwr_err, TAG, "Failed to init on-chip LDO SD power control");
    }

    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < (sizeof(SD_MOUNT_PROFILES) / sizeof(SD_MOUNT_PROFILES[0])); i++) {
        const sd_mount_profile_t *profile = &SD_MOUNT_PROFILES[i];

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = profile->max_freq_khz;
        host.pwr_ctrl_handle = pwr_ctrl;
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        host.init = shared_sdmmc_host_init;
        host.deinit = shared_sdmmc_host_deinit;
#endif

        sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_cfg.width = profile->width;
        slot_cfg.clk = SDMMC_PIN_CLK;
        slot_cfg.cmd = SDMMC_PIN_CMD;
        slot_cfg.d0 = SDMMC_PIN_D0;
        slot_cfg.d1 = SDMMC_PIN_D1;
        slot_cfg.d2 = SDMMC_PIN_D2;
        slot_cfg.d3 = SDMMC_PIN_D3;
        slot_cfg.cd = SDMMC_SLOT_NO_CD;
        slot_cfg.wp = SDMMC_SLOT_NO_WP;
        /* Match Espressif's SDMMC guidance and the known-stable camera design:
         * weak internal pull-ups supplement the board pull-ups during sustained
         * USB MSC reads, where marginal CMD/DAT lines otherwise surface as
         * intermittent Windows "device is not ready" failures. */
        slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        ESP_LOGI(TAG, "Trying SD mount: LDO VO4 + %s", profile->label);
        last_err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_config, out_card);
        if (last_err == ESP_OK) {
            s_card = out_card != NULL ? *out_card : NULL;
            s_sd_mounted = true;
            s_sd_last_err = ESP_OK;
            event_breadcrumb_record(BREADCRUMB_SD_MOUNT, (int32_t)(i + 1U));
            ESP_LOGI(TAG, "SD mount success: %s", profile->label);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "SD mount failed with %s: %s", profile->label, esp_err_to_name(last_err));
    }

    (void)sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl);
    (void)sd_storage_set_card_power(false);
    s_sd_mounted = false;
    s_card = NULL;
    s_sd_last_err = last_err;
    event_breadcrumb_record(BREADCRUMB_SD_MOUNT, -(int32_t)last_err);
    return last_err;
}

void sd_storage_unmount(sdmmc_card_t *card)
{
    sd_pwr_ctrl_handle_t pwr_ctrl = card ? card->host.pwr_ctrl_handle : NULL;

    if (card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    }

    if (pwr_ctrl) {
        esp_err_t pwr_err = sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl);
        if (pwr_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to release LDO VO4 SD power control: %s", esp_err_to_name(pwr_err));
        }
    }

    (void)sd_storage_set_card_power(false);

    if (s_sd_mutex != NULL) {
        vSemaphoreDelete(s_sd_mutex);
        s_sd_mutex = NULL;
    }

    s_sd_mounted = false;
    s_usb_owned = false;
    s_card = NULL;
    s_sd_last_err = ESP_ERR_INVALID_STATE;
    s_atomic_recovery_done = false;
}

esp_err_t sd_storage_ensure_capture_dir(void)
{
    struct stat st = {0};
    if (stat(SD_CAPTURE_DIR, &st) == 0) {
        if (!s_atomic_recovery_done) {
            esp_err_t err = sd_storage_recover_atomic_files(SD_CAPTURE_DIR);
            if (err != ESP_OK) return err;
            s_atomic_recovery_done = true;
        }
        return ESP_OK;
    }

    if (mkdir(SD_CAPTURE_DIR, 0775) == 0) {
        ESP_LOGI(TAG, "Created capture dir: %s", SD_CAPTURE_DIR);
        s_atomic_recovery_done = true;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to create %s (errno=%d: %s)", SD_CAPTURE_DIR, errno, strerror(errno));
    return ESP_FAIL;
}

bool sd_storage_is_mounted(void)
{
    return s_sd_mounted;
}

bool sd_storage_is_usb_owned(void)
{
    return s_usb_owned;
}

void sd_storage_set_usb_owned(bool usb_owned)
{
    s_usb_owned = usb_owned;
    s_sd_mounted = !usb_owned;
    s_sd_last_err = usb_owned ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t sd_storage_release_filesystem(void)
{
    ESP_RETURN_ON_FALSE(s_sd_mounted && !s_usb_owned && s_card != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "SD filesystem is not application-owned");

    /* Keep the initialized SDMMC card/host alive. TinyUSB needs the card
     * descriptor for raw sector I/O, but FATFS/VFS must no longer access it. */
    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    ESP_RETURN_ON_FALSE(pdrv != 0xff, ESP_ERR_INVALID_STATE, TAG,
                        "SD card is not registered with FATFS");
    char drive[3] = {(char)('0' + pdrv), ':', '\0'};
    FRESULT fr = f_mount(NULL, drive, 0);
    ESP_RETURN_ON_FALSE(fr == FR_OK, ESP_FAIL, TAG, "Failed to unmount FATFS drive");
    ESP_RETURN_ON_ERROR(esp_vfs_fat_unregister_path(SD_MOUNT_POINT), TAG,
                        "Failed to unregister SD VFS path");
    ff_diskio_unregister(pdrv);
    s_sd_mounted = false;
    s_sd_last_err = ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t sd_storage_last_error(void)
{
    return s_sd_mounted ? ESP_OK : s_sd_last_err;
}

esp_err_t sd_storage_require_mounted(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }
    return s_sd_last_err != ESP_OK ? s_sd_last_err : ESP_ERR_INVALID_STATE;
}

void sd_storage_lock(void)
{
    if (s_sd_mutex != NULL) {
        xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    }
}

void sd_storage_unlock(void)
{
    if (s_sd_mutex != NULL) {
        xSemaphoreGive(s_sd_mutex);
    }
}

static bool path_has_suffix(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
}

esp_err_t sd_storage_recover_atomic_files(const char *directory)
{
    if (directory == NULL) return ESP_ERR_INVALID_ARG;
    DIR *dir = opendir(directory);
    if (dir == NULL) return ESP_FAIL;

    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        bool is_temp = path_has_suffix(entry->d_name, ".tmp");
        bool is_backup = path_has_suffix(entry->d_name, ".bak");
        if (!is_temp && !is_backup) continue;

        char artifact[160];
        if (snprintf(artifact, sizeof(artifact), "%s/%s", directory, entry->d_name) >=
            (int)sizeof(artifact)) {
            result = ESP_ERR_INVALID_SIZE;
            continue;
        }
        if (is_temp) {
            if (unlink(artifact) == 0) {
                ESP_LOGW(TAG, "Removed interrupted atomic write: %s", artifact);
            }
            continue;
        }

        char final_path[160];
        strlcpy(final_path, artifact, sizeof(final_path));
        final_path[strlen(final_path) - 4] = '\0';
        struct stat final_stat;
        if (stat(final_path, &final_stat) == 0) {
            if (unlink(artifact) == 0) {
                ESP_LOGW(TAG, "Removed completed atomic backup: %s", artifact);
            }
        } else if (rename(artifact, final_path) == 0) {
            ESP_LOGW(TAG, "Restored interrupted atomic backup: %s", final_path);
        } else {
            result = ESP_FAIL;
        }
    }
    closedir(dir);
    return result;
}

esp_err_t sd_storage_atomic_begin(sd_storage_atomic_file_t *atomic_file,
                                  const char *final_path,
                                  const char *mode)
{
    /* Hold the global SD lock until commit/abort. Writers stage into a sibling
     * temporary file so readers never observe a partially written artifact. */
    if (atomic_file == NULL || final_path == NULL || mode == NULL ||
        strchr(mode, 'w') == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(atomic_file, 0, sizeof(*atomic_file));
    if (strlcpy(atomic_file->final_path, final_path,
                sizeof(atomic_file->final_path)) >= sizeof(atomic_file->final_path) ||
        snprintf(atomic_file->temp_path, sizeof(atomic_file->temp_path),
                 "%s.tmp", final_path) >= (int)sizeof(atomic_file->temp_path) ||
        snprintf(atomic_file->backup_path, sizeof(atomic_file->backup_path),
                 "%s.bak", final_path) >= (int)sizeof(atomic_file->backup_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    sd_storage_lock();
    (void)unlink(atomic_file->temp_path);
    atomic_file->file = fopen(atomic_file->temp_path, mode);
    if (atomic_file->file == NULL) {
        sd_storage_unlock();
        return ESP_FAIL;
    }
    atomic_file->active = true;
    return ESP_OK;
}

void sd_storage_atomic_abort(sd_storage_atomic_file_t *atomic_file)
{
    if (atomic_file == NULL || !atomic_file->active) return;
    if (atomic_file->file != NULL) fclose(atomic_file->file);
    (void)unlink(atomic_file->temp_path);
    atomic_file->file = NULL;
    atomic_file->active = false;
    sd_storage_unlock();
}

esp_err_t sd_storage_atomic_commit(sd_storage_atomic_file_t *atomic_file)
{
    if (atomic_file == NULL || !atomic_file->active || atomic_file->file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    if (fflush(atomic_file->file) != 0) result = ESP_FAIL;
    if (fsync(fileno(atomic_file->file)) != 0) result = ESP_FAIL;
    if (fclose(atomic_file->file) != 0) result = ESP_FAIL;
    atomic_file->file = NULL;

    bool had_final = false;
    struct stat final_stat;
    if (result == ESP_OK && stat(atomic_file->final_path, &final_stat) == 0) {
        (void)unlink(atomic_file->backup_path);
        if (rename(atomic_file->final_path, atomic_file->backup_path) != 0) {
            result = ESP_FAIL;
        } else {
            had_final = true;
        }
    }
    if (result == ESP_OK &&
        rename(atomic_file->temp_path, atomic_file->final_path) != 0) {
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        if (had_final) (void)unlink(atomic_file->backup_path);
        event_breadcrumb_record(BREADCRUMB_SD_COMMIT, 0);
    } else {
        (void)unlink(atomic_file->temp_path);
        if (had_final) {
            (void)rename(atomic_file->backup_path, atomic_file->final_path);
        }
        event_breadcrumb_record(BREADCRUMB_SD_COMMIT, (int32_t)result);
    }

    atomic_file->active = false;
    sd_storage_unlock();
    return result;
}

esp_err_t sd_storage_benchmark_write(size_t total_bytes,
                                     size_t block_bytes,
                                     sd_storage_bench_result_t *out_result)
{
    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }

    ESP_RETURN_ON_ERROR(sd_storage_require_mounted(), TAG, "SD not mounted");
    if (block_bytes == 0 || total_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buffer = heap_caps_aligned_alloc(64, block_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < block_bytes; ++i) {
        buffer[i] = (uint8_t)i;
    }

    bool dma_aligned = sdmmc_host_check_buffer_alignment(SDMMC_HOST_SLOT_0, buffer, block_bytes);

    sd_storage_lock();
    FILE *file = fopen(SD_BENCH_PATH, "wb");
    if (file == NULL) {
        sd_storage_unlock();
        heap_caps_free(buffer);
        return ESP_FAIL;
    }
    setvbuf(file, NULL, _IONBF, 0);

    size_t written_total = 0;
    int64_t start_us = esp_timer_get_time();
    esp_err_t err = ESP_OK;

    while (written_total < total_bytes) {
        size_t n = total_bytes - written_total;
        if (n > block_bytes) {
            n = block_bytes;
        }

        size_t written = fwrite(buffer, 1, n, file);
        written_total += written;
        if (written != n) {
            err = ESP_FAIL;
            break;
        }
    }

    if (fflush(file) != 0) {
        err = ESP_FAIL;
    }
    fclose(file);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    remove(SD_BENCH_PATH);
    sd_storage_unlock();

    if (out_result != NULL) {
        out_result->bytes_written = written_total;
        out_result->elapsed_ms = (uint32_t)(elapsed_us / 1000);
        out_result->block_bytes = (uint32_t)block_bytes;
        out_result->dma_aligned = dma_aligned;
    }

    ESP_LOGI(TAG,
             "SD benchmark: %zu bytes in %" PRId64 " ms, block=%zu, dma_aligned=%s",
             written_total,
             elapsed_us / 1000,
             block_bytes,
             dma_aligned ? "yes" : "NO");

    heap_caps_free(buffer);
    return err;
}
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"

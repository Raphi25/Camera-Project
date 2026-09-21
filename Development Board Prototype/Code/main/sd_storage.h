/* Single-owner SD filesystem contract shared by capture, logs, and transfer. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT "/sdcard"
#define SD_CAPTURE_DIR SD_MOUNT_POINT "/captures"

/*
 * SD-card storage facade.
 *
 * Centralizes the board-specific SDMMC pins, LDO power control, mount fallback
 * profiles, and a process-wide mutex.  Other modules should use sd_storage_lock
 * around multi-step file operations so image transfer, logging, and writer task
 * activity do not interleave at the filesystem level.
 */

/* Mount the FAT filesystem and return the ESP-IDF card handle. */
esp_err_t sd_storage_mount(sdmmc_card_t **out_card);

/* Unmount storage and destroy the module mutex. */
void sd_storage_unmount(sdmmc_card_t *card);

/* Ensure SD_CAPTURE_DIR exists after mounting. */
esp_err_t sd_storage_ensure_capture_dir(void);

/* Fast readiness checks for command handlers and background tasks. */
bool sd_storage_is_mounted(void);
esp_err_t sd_storage_last_error(void);
esp_err_t sd_storage_require_mounted(void);

/* Transfer FATFS ownership to TinyUSB without shutting down the SDMMC host. */
esp_err_t sd_storage_release_filesystem(void);
void sd_storage_set_usb_owned(bool usb_owned);
bool sd_storage_is_usb_owned(void);

/* Coarse filesystem mutex shared by camera, serial transfer, and log sink. */
void sd_storage_lock(void);
void sd_storage_unlock(void);

typedef struct {
    FILE *file;
    char final_path[128];
    char temp_path[136];
    char backup_path[136];
    bool active;
} sd_storage_atomic_file_t;

/* Whole-file transaction. begin() holds the SD mutex until commit/abort. */
esp_err_t sd_storage_atomic_begin(sd_storage_atomic_file_t *atomic_file,
                                  const char *final_path,
                                  const char *mode);
esp_err_t sd_storage_atomic_commit(sd_storage_atomic_file_t *atomic_file);
void sd_storage_atomic_abort(sd_storage_atomic_file_t *atomic_file);
esp_err_t sd_storage_recover_atomic_files(const char *directory);

typedef struct {
    size_t bytes_written;
    uint32_t elapsed_ms;
    uint32_t block_bytes;
    bool dma_aligned;
} sd_storage_bench_result_t;

/* Sequential write benchmark using the mounted app SD path. */
esp_err_t sd_storage_benchmark_write(size_t total_bytes,
                                     size_t block_bytes,
                                     sd_storage_bench_result_t *out_result);

/* Boot-time NVS setup and persistent camera-mode selection. */

#include "startup_init.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ov5647.h"
#include "sc2336.h"

#define CAMERA_MODE_NVS_NAMESPACE "device"
#define CAMERA_MODE_NVS_KEY       "camera_fps"

static const char *TAG = "startup_init";
static uint8_t s_camera_mode_fps = 15;

static esp_err_t apply_camera_mode(uint8_t ov5647_fps)
{
    const uint8_t sc2336_fps = ov5647_fps == 45 ? 30 : 25;

    ESP_RETURN_ON_ERROR(ov5647_set_boot_fps(ov5647_fps), TAG,
                        "select OV5647 boot mode");
    ESP_RETURN_ON_ERROR(sc2336_set_boot_fps(sc2336_fps), TAG,
                        "select SC2336 boot mode");
    return ESP_OK;
}

const char *startup_camera_mode_name(uint8_t fps)
{
    return fps == 45 ? "NORMAL" : "LOW_LIGHT";
}

uint8_t startup_camera_mode_fps(void)
{
    return s_camera_mode_fps;
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t startup_init_camera_mode(void)
{
    ESP_RETURN_ON_ERROR(initialize_nvs(), TAG, "initialize NVS");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CAMERA_MODE_NVS_NAMESPACE, NVS_READONLY, &handle);
    uint8_t fps = 15;
    if (err == ESP_OK) {
        esp_err_t read_err = nvs_get_u8(handle, CAMERA_MODE_NVS_KEY, &fps);
        nvs_close(handle);
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) {
            return read_err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    if (fps != 15 && fps != 45) {
        ESP_LOGW(TAG, "Invalid stored camera mode fps=%u; using LOW_LIGHT", fps);
        fps = 15;
    }
    ESP_RETURN_ON_ERROR(apply_camera_mode(fps), TAG, "select sensor boot modes");
    s_camera_mode_fps = fps;
    ESP_LOGI(TAG, "Camera mode: %s (OV5647=%u fps, SC2336=%u fps, IMX500=30 fps)",
             startup_camera_mode_name(fps), fps, fps == 45 ? 30 : 25);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t startup_select_camera_mode(uint8_t fps)
{
    if (fps != 15 && fps != 45) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(CAMERA_MODE_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open camera mode NVS");
    esp_err_t err = nvs_set_u8(handle, CAMERA_MODE_NVS_KEY, fps);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        s_camera_mode_fps = fps;
        if (xTaskCreate(restart_task, "camera_restart", 2048, NULL, 10, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return err;
}

/* TinyUSB Mass Storage mode for direct, high-speed access to captured media. */
#include "usb_msc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sd_storage.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "usb_msc";
static tinyusb_msc_storage_handle_t s_storage;
static bool s_usb_driver_installed;
static bool s_active;

esp_err_t usb_msc_init_detection(void)
{
    if (s_usb_driver_installed) return ESP_OK;

    /* Enumerate the native OTG port without lending the SD card to the host.
     * With no MSC storage mapped, TinyUSB correctly reports "no medium" while
     * tud_connected() still tells STATUS whether an OTG host is attached. */
    tinyusb_config_t usb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&usb_cfg), TAG,
                        "Failed to initialize TinyUSB OTG detection");
    s_usb_driver_installed = true;
    ESP_LOGI(TAG, "OTG detection ready; SD card remains application-owned");
    return ESP_OK;
}

static void storage_event(tinyusb_msc_storage_handle_t handle,
                          tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    if (event->id != TINYUSB_MSC_EVENT_MOUNT_COMPLETE) return;
    bool usb_owned = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB;
    sd_storage_set_usb_owned(usb_owned);
    s_active = usb_owned;
    ESP_LOGI(TAG, "SD ownership transferred to %s", usb_owned ? "USB host" : "application");
}

esp_err_t usb_msc_start(sdmmc_card_t *card)
{
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_INVALID_ARG, TAG, "Missing SD card handle");
    if (s_active) return ESP_OK;

    if (s_storage == NULL) {
        ESP_RETURN_ON_ERROR(sd_storage_release_filesystem(), TAG,
                            "Failed to release application FATFS mount");
        tinyusb_msc_driver_config_t driver_cfg = {
            .user_flags.auto_mount_off = 1,
            .callback = storage_event,
        };
        ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_cfg), TAG,
                            "Failed to install TinyUSB MSC driver");
        tinyusb_msc_storage_config_t storage_cfg = {
            .medium.card = card,
            .fat_fs = {
                .base_path = SD_MOUNT_POINT,
                .config = {
                    .format_if_mount_failed = false,
                    .max_files = 5,
                    .allocation_unit_size = 16 * 1024,
                },
                .do_not_format = true,
                .format_flags = 0,
            },
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        };
        esp_err_t err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
        if (err != ESP_OK) {
            (void)tinyusb_msc_uninstall_driver();
            return err;
        }
        err = usb_msc_init_detection();
        if (err != ESP_OK) {
            (void)tinyusb_msc_delete_storage(s_storage);
            s_storage = NULL;
            (void)tinyusb_msc_uninstall_driver();
            return err;
        }
    } else {
        ESP_RETURN_ON_ERROR(usb_msc_init_detection(), TAG,
                            "Failed to initialize the native OTG port");
        ESP_RETURN_ON_ERROR(tinyusb_msc_set_storage_mount_point(
                                s_storage, TINYUSB_MSC_STORAGE_MOUNT_USB),
                            TAG, "Failed to transfer SD ownership to USB");
    }
    sd_storage_set_usb_owned(true);
    s_active = true;
    ESP_LOGI(TAG, "USB Mass Storage ready on the native OTG USB-C port");
    return ESP_OK;
}

esp_err_t usb_msc_stop(void)
{
    ESP_RETURN_ON_FALSE(s_storage != NULL && s_usb_driver_installed,
                        ESP_ERR_INVALID_STATE, TAG, "USB MSC is not initialized");
    ESP_RETURN_ON_ERROR(tinyusb_msc_set_storage_mount_point(
                            s_storage, TINYUSB_MSC_STORAGE_MOUNT_APP),
                        TAG, "Failed to return SD ownership to application");
    tinyusb_msc_mount_point_t owner;
    ESP_RETURN_ON_ERROR(tinyusb_msc_get_storage_mount_point(s_storage, &owner), TAG,
                        "Failed to verify SD ownership");
    ESP_RETURN_ON_FALSE(owner == TINYUSB_MSC_STORAGE_MOUNT_APP, ESP_FAIL, TAG,
                        "SD ownership did not return to application");
    sd_storage_set_usb_owned(false);
    s_active = false;
    return sd_storage_ensure_capture_dir();
}

bool usb_msc_is_active(void) { return s_active; }
bool usb_msc_is_connected(void)
{
    return s_usb_driver_installed && tud_connected();
}

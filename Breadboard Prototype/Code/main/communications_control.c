/* Board-level USB detection and ESP32-C6 reset/power controls. */

#include "communications_control.h"

#include "camera.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* FireBeetle 2 ESP32-P4 on-board C6 reset wiring. Some esp-hosted Kconfig
 * versions hide the SDIO-specific symbols until their transport is selected. */
#ifndef CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE
#define CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE 54
#endif
#ifndef CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH
#define CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH 1
#endif

static const char *TAG = "communications";

bool communications_usb_detect_is_configured(void)
{
    return CAMERA_USB_DETECT_GPIO != GPIO_NUM_NC;
}

static void configure_usb_detect_gpio(void)
{
#if CAMERA_USB_DETECT_GPIO != GPIO_NUM_NC
    gpio_config_t usb_cfg = {
        .pin_bit_mask = 1ULL << CAMERA_USB_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&usb_cfg);
    (void)gpio_sleep_set_direction(CAMERA_USB_DETECT_GPIO, GPIO_MODE_INPUT);
    (void)gpio_sleep_sel_en(CAMERA_USB_DETECT_GPIO);
#endif
}

bool communications_usb_detect_is_active(void)
{
    if (!communications_usb_detect_is_configured()) {
        return false;
    }
    configure_usb_detect_gpio();
    return gpio_get_level(CAMERA_USB_DETECT_GPIO) == CAMERA_USB_DETECT_ACTIVE_LEVEL;
}

void communications_hold_coprocessor_in_reset(void)
{
#if CONFIG_ESP_HOSTED_ENABLED
    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&reset_cfg) == ESP_OK) {
#if CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH
        (void)gpio_set_level(CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, 1);
#else
        (void)gpio_set_level(CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE, 0);
#endif
        ESP_LOGI(TAG, "C6 held in reset for autonomous timer-wake power saving");
    }
#endif
}

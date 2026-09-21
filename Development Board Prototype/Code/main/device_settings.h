/* Board pin assignments and capture/storage policy constants. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_types.h"

/* Central device settings. Change these values before rebuilding firmware. */

/* Camera and image quality */
#define CAMERA_OUTPUT_RGB888             1
#define CAMERA_JPEG_QUALITY              90
#define CAMERA_JPEG_TIMEOUT_MS           30000
#define CAMERA_STREAM_WARMUP_FRAMES      50
#define CAMERA_ISP_BRIGHTNESS_PERCENT    54
#define CAMERA_JPEG_BACKEND_M2M          0

/* Capture scheduling and burst behavior */
#define CAMERA_BURST_WINDOW_MS           3000U
#define CAMERA_BURST_IMAGE_COUNT         3U
#define CAMERA_AUTO_SNAP_INTERVAL_MS     30000U
#define CAMERA_AUTO_SNAP_DEEP_SLEEP      1
#define CAMERA_AUTO_SNAP_INITIAL_IDLE_MS 1000U
#define INITIAL_WAKE_ESTIMATE_MS         1500U
#define AUTO_SNAP_INTERVAL_MIN_MS        5000U
#define AUTO_SNAP_INTERVAL_MAX_MS        3600000U
#define BOOT_TO_CAPTURE_EST_MIN_MS       500U
#define BOOT_TO_CAPTURE_EST_MAX_MS       20000U
#define CAPTURE_INTERVAL_FEEDBACK_DIVISOR 2U
#define CAPTURE_INTERVAL_MAX_ADJUST_MS    500U

/* Board pins */
#define CAMERA_USER_LED_GPIO             GPIO_NUM_3
#define CAMERA_USER_LED_ACTIVE_LEVEL     1
#define CAMERA_USER_LED_BLINK_MS          100U
#define CAMERA_PWDN_GPIO                 -1
#define CAMERA_BUTTON_GPIO               GPIO_NUM_5
#define CAMERA_BUTTON_ACTIVE_LEVEL       0
#define CAMERA_BUTTON_DEBOUNCE_MS         40U
#define CAMERA_BUTTON_POLL_MS             20U
#define CAMERA_USB_DETECT_GPIO            GPIO_NUM_NC
#define CAMERA_USB_DETECT_ACTIVE_LEVEL    1
#define RTC_I2C_SDA_GPIO                  GPIO_NUM_7
#define RTC_I2C_SCL_GPIO                  GPIO_NUM_8
#define RTC_I2C_PORT_SPEED_HZ             100000U

/* BMI323 orientation mapping. Device +Y points toward the wearable's top and
 * device +Z points out through the camera lens. Axis values are 0=X, 1=Y,
 * 2=Z; change these mappings/signs after a physical six-position check if the
 * breakout is mounted in another rotation. */
#define IMU_DEVICE_X_SENSOR_AXIS           0
#define IMU_DEVICE_X_SENSOR_SIGN           1
#define IMU_DEVICE_Y_SENSOR_AXIS           1
#define IMU_DEVICE_Y_SENSOR_SIGN           1
#define IMU_DEVICE_Z_SENSOR_AXIS           2
#define IMU_DEVICE_Z_SENSOR_SIGN           1
#define IMU_ORIENTATION_CARDINAL_THRESHOLD_MG 800
#define IMU_I2C_PORT                       I2C_NUM_1
#define IMU_I2C_SDA_GPIO                   GPIO_NUM_20
#define IMU_I2C_SCL_GPIO                   GPIO_NUM_36
#define IMU_I2C_PORT_SPEED_HZ              100000U
#define IMU_INT1_GPIO                      GPIO_NUM_22
#define IMU_INT2_GPIO                      GPIO_NUM_21

/* Optional P4/C6 bridge */
#define CAMERA_ESP_HOSTED_BLE_ENABLE     0
#define CAMERA_C6_BLE_BRIDGE_ENABLE      0
#define CAMERA_C6_BRIDGE_UART_PORT       1
#define CAMERA_C6_BRIDGE_UART_BAUD       115200
#define CAMERA_C6_BRIDGE_TX_GPIO         GPIO_NUM_NC
#define CAMERA_C6_BRIDGE_RX_GPIO         GPIO_NUM_NC
#define CAMERA_C6_WAKEUP_GPIO             GPIO_NUM_6
#define CAMERA_COMMS_START_ON_TIMER_WAKE   0

/* Storage, logging, and transfers */
#define DEVICE_LOG_INTERVAL_MS           600000U
#define DEVICE_SD_RETRY_MS               2000U
#define DEVICE_IMAGE_TX_CHUNK_BYTES      8192U
#define DEVICE_IMAGE_BINARY_CHUNK_BYTES  32768U
#define DEVICE_SD_CAPTURE_PATH            "/sdcard/captures"

typedef struct {
    uint32_t capture_interval_ms;
    uint32_t burst_window_ms;
    uint32_t initial_wake_estimate_ms;
    uint32_t jpeg_quality;
    uint32_t isp_brightness_percent;
    bool deep_sleep_enabled;
    const char *sd_capture_path;
} device_settings_t;

extern const device_settings_t g_device_settings;

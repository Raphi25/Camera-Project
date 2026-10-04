/*
 * SPDX-FileCopyrightText: 2026 Arducam Electronic Technology (Nanjing) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_cam_sensor_types.h"
#include "arducam_imx500_types.h"

#define ARDUCAM_IMX500_SENSOR_NAME "Arducam_IMX500"
#define ARDUCAM_IMX500_PID         0x00000030
#define ARDUCAM_IMX500_SCCB_ADDR   0x0c

/**
 * @brief Power on camera sensor device and detect the device connected to the designated sccb bus.
 *
 * @param[in] config Configuration related to device power-on and detection.
 * @return
 *      - Camera device handle on success, otherwise, failed.
 */
esp_cam_sensor_device_t *arducam_imx500_detect(esp_cam_sensor_config_t *config);

/** Select 30 fps (normal) or 25 fps (low light) before sensor detection. */
esp_err_t arducam_imx500_set_boot_fps(uint8_t fps);

#ifdef __cplusplus
}
#endif

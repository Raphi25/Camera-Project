/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_cam_sensor_types.h"

#define OV5647_SCCB_ADDR   0x36
#define OV5647_PID         0x5647
#define OV5647_SENSOR_NAME "OV5647"

/**
 * @brief Select the 1280x960 boot format by frame rate.
 *
 * Call before sensor detection. Supported values are 25 (low light) and
 * 30 (normal). The selection remains active until the next software reset.
 */
esp_err_t ov5647_set_boot_fps(uint8_t fps);

/**
 * @brief Power on camera sensor device and detect the device connected to the designated sccb bus.
 *
 * @param[in] config Configuration related to device power-on and detection.
 * @return
 *      - Camera device handle on success, otherwise, failed.
 */
esp_cam_sensor_device_t *ov5647_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif

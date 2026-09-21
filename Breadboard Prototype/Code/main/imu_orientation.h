/* BMI323 accelerometer sampling and wearable-orientation classification. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define IMU_ORIENTATION_STATUS_MAX 48

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t address;
    bool ready;
} imu_orientation_t;

typedef struct {
    char status[IMU_ORIENTATION_STATUS_MAX];
    int16_t x_mg;
    int16_t y_mg;
    int16_t z_mg;
    bool valid;
} imu_orientation_sample_t;

esp_err_t imu_orientation_init(imu_orientation_t *imu,
                               i2c_port_t i2c_port,
                               gpio_num_t sda_gpio,
                               gpio_num_t scl_gpio,
                               uint32_t i2c_speed_hz);

void imu_orientation_deinit(imu_orientation_t *imu);

esp_err_t imu_orientation_read(imu_orientation_t *imu,
                               imu_orientation_sample_t *sample);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool is_dark;
    uint8_t dark_count;
    uint8_t bright_count;
} scene_classifier_t;

typedef struct {
    uint32_t avg_luma;
    uint32_t dark_pct;
    bool is_dark;
} scene_stats_t;

esp_err_t scene_classifier_init(scene_classifier_t *classifier);
void scene_classifier_update_from_uyvy(scene_classifier_t *classifier, const uint8_t *uyvy, size_t uyvy_size, scene_stats_t *stats);
void scene_classifier_update_from_rgb565(scene_classifier_t *classifier, const uint8_t *rgb565,
                                         size_t rgb565_size, scene_stats_t *stats);

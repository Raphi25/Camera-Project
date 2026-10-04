#include "scene_classifier.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

/* Hysteresis thresholds: darker criteria to turn ON, brighter criteria to turn OFF. */
#define SCENE_ON_AVG_LUMA_THRESHOLD            (30)
#define SCENE_OFF_AVG_LUMA_THRESHOLD           (50)
#define SCENE_DARK_PIXEL_LUMA_THRESHOLD        (15)
#define SCENE_ON_DARK_PIXEL_PERCENT_THRESHOLD  (98)
#define SCENE_OFF_DARK_PIXEL_PERCENT_THRESHOLD (90)
#define SCENE_CONSEC_DARK_FRAMES_TO_ON         (2)
#define SCENE_CONSEC_BRIGHT_FRAMES_TO_OFF      (2)
#define SCENE_ANALYSIS_PAIR_STRIDE             (8)

static const char *TAG = "scene_classifier";

esp_err_t scene_classifier_init(scene_classifier_t *classifier)
{
    if (classifier == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(classifier, 0, sizeof(*classifier));
    ESP_LOGI(TAG, "Light/Dark classification enabled (no LED control)");
    return ESP_OK;
}

static void scene_classifier_apply(scene_classifier_t *classifier, uint32_t avg_luma,
                                   uint32_t dark_pct, scene_stats_t *stats)
{
    if (classifier == NULL) return;
    if (stats) {
        stats->avg_luma = avg_luma;
        stats->dark_pct = dark_pct;
    }

    bool is_dark = (avg_luma <= SCENE_ON_AVG_LUMA_THRESHOLD) ||
                   (dark_pct >= SCENE_ON_DARK_PIXEL_PERCENT_THRESHOLD);
    bool is_bright = (avg_luma >= SCENE_OFF_AVG_LUMA_THRESHOLD) &&
                     (dark_pct <= SCENE_OFF_DARK_PIXEL_PERCENT_THRESHOLD);
    if (is_dark) {
        if (classifier->dark_count < SCENE_CONSEC_DARK_FRAMES_TO_ON) classifier->dark_count++;
        classifier->bright_count = 0;
    } else if (is_bright) {
        if (classifier->bright_count < SCENE_CONSEC_BRIGHT_FRAMES_TO_OFF) classifier->bright_count++;
        classifier->dark_count = 0;
    } else {
        classifier->dark_count = 0;
        classifier->bright_count = 0;
    }

    if (!classifier->is_dark && classifier->dark_count >= SCENE_CONSEC_DARK_FRAMES_TO_ON) {
        classifier->is_dark = true;
        ESP_LOGI(TAG, "SCENE: DARK (avgY=%" PRIu32 ", dark%%=%" PRIu32 ")", avg_luma, dark_pct);
        classifier->dark_count = 0;
        classifier->bright_count = 0;
    } else if (classifier->is_dark && classifier->bright_count >= SCENE_CONSEC_BRIGHT_FRAMES_TO_OFF) {
        classifier->is_dark = false;
        ESP_LOGI(TAG, "SCENE: LIGHT (avgY=%" PRIu32 ", dark%%=%" PRIu32 ")", avg_luma, dark_pct);
        classifier->dark_count = 0;
        classifier->bright_count = 0;
    }
    if (stats) stats->is_dark = classifier->is_dark;
}

void scene_classifier_update_from_uyvy(scene_classifier_t *classifier, const uint8_t *uyvy, size_t uyvy_size, scene_stats_t *stats)
{
    if (classifier == NULL || uyvy == NULL || uyvy_size < 4) {
        return;
    }

    size_t pair_count = uyvy_size / 4;
    if (pair_count == 0) {
        return;
    }

    uint64_t luma_sum = 0;
    uint32_t pixel_count = 0;
    uint32_t dark_pixels = 0;

    /* UYVY packs 2 pixels into 4 bytes: [U, Y0, V, Y1]. */
    /* Stride sampling reduces CPU usage while keeping enough signal for dark/light decisions. */
    for (size_t i = 0; i < pair_count; i += SCENE_ANALYSIS_PAIR_STRIDE) {
        const uint8_t *p = uyvy + (i * 4);
        uint8_t y0 = p[1];
        uint8_t y1 = p[3];
        luma_sum += y0;
        luma_sum += y1;
        dark_pixels += (y0 <= SCENE_DARK_PIXEL_LUMA_THRESHOLD) ? 1 : 0;
        dark_pixels += (y1 <= SCENE_DARK_PIXEL_LUMA_THRESHOLD) ? 1 : 0;
        pixel_count += 2;
    }

    if (pixel_count == 0) {
        return;
    }

    uint32_t avg_luma = (uint32_t)(luma_sum / pixel_count);
    uint32_t dark_pct = (dark_pixels * 100U) / pixel_count;

    scene_classifier_apply(classifier, avg_luma, dark_pct, stats);
}

void scene_classifier_update_from_rgb565(scene_classifier_t *classifier, const uint8_t *rgb565,
                                         size_t rgb565_size, scene_stats_t *stats)
{
    if (classifier == NULL || rgb565 == NULL || rgb565_size < 2) return;

    uint64_t luma_sum = 0;
    uint32_t pixel_count = 0;
    uint32_t dark_pixels = 0;
    size_t pixel_total = rgb565_size / 2;
    /* RGB565 is little-endian on ESP32-P4. Sample every eighth pixel and use
     * an integer BT.601 approximation to obtain an 8-bit luma value. */
    for (size_t i = 0; i < pixel_total; i += SCENE_ANALYSIS_PAIR_STRIDE) {
        uint16_t pixel = (uint16_t)rgb565[i * 2] | ((uint16_t)rgb565[i * 2 + 1] << 8);
        uint32_t red = ((pixel >> 11) & 0x1FU) * 255U / 31U;
        uint32_t green = ((pixel >> 5) & 0x3FU) * 255U / 63U;
        uint32_t blue = (pixel & 0x1FU) * 255U / 31U;
        uint32_t luma = (77U * red + 150U * green + 29U * blue) >> 8;
        luma_sum += luma;
        dark_pixels += luma <= SCENE_DARK_PIXEL_LUMA_THRESHOLD ? 1U : 0U;
        pixel_count++;
    }
    if (pixel_count == 0) return;
    scene_classifier_apply(classifier, (uint32_t)(luma_sum / pixel_count),
                           (dark_pixels * 100U) / pixel_count, stats);
}

void scene_classifier_update_from_rgb888(scene_classifier_t *classifier, const uint8_t *rgb888,
                                         size_t rgb888_size, scene_stats_t *stats)
{
    if (classifier == NULL || rgb888 == NULL || rgb888_size < 3) return;

    uint64_t luma_sum = 0;
    uint32_t pixel_count = 0;
    uint32_t dark_pixels = 0;
    size_t pixel_total = rgb888_size / 3;
    /* V4L2 RGB24 is packed R,G,B. Use the same sampling density and BT.601
     * luma approximation as RGB565, without reducing color precision first. */
    for (size_t i = 0; i < pixel_total; i += SCENE_ANALYSIS_PAIR_STRIDE) {
        const uint8_t *pixel = rgb888 + i * 3;
        uint32_t luma = (77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8;
        luma_sum += luma;
        dark_pixels += luma <= SCENE_DARK_PIXEL_LUMA_THRESHOLD ? 1U : 0U;
        pixel_count++;
    }
    if (pixel_count == 0) return;
    scene_classifier_apply(classifier, (uint32_t)(luma_sum / pixel_count),
                           (dark_pixels * 100U) / pixel_count, stats);
}

#include "blur.h"
#include <math.h>
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t lock;
static bool sweeping;
static unsigned low = 1300, high = 1700, period = 2000;
static int64_t start, stop_at;
static const char *TAG = "servo";

static esp_err_t pulse(unsigned us)
{
    uint32_t duty = (us * 16384UL + 10000) / 20000;
    /* Direct updates avoid the LEDC fade service; the servo task provides its
       own 20 ms motion interpolation. */
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void servo_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        if (sweeping && stop_at && esp_timer_get_time()>=stop_at) {
            sweeping=false;
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        }
        if (sweeping) {
            uint32_t phase = ((esp_timer_get_time() - start) / 1000) % period;
            /* Ease in/out at each reversal, approximating a gentle head pan. */
            float fraction=0.5f-0.5f*cosf(6.28318530718f*(float)phase/(float)period);
            unsigned us=low+(unsigned)((high-low)*fraction+0.5f);
            if (pulse(us) != ESP_OK) {
                sweeping = false;
                ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            }
        }
        xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t servo_init(void)
{
    lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ledc_timer_config_t timer = {.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_14_BIT, .timer_num=LEDC_TIMER_0,
        .freq_hz=50, .clk_cfg=LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer");
    ledc_channel_config_t channel = {.gpio_num=32, .speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0, .timer_sel=LEDC_TIMER_0, .duty=0};
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "GPIO32 PWM");
    ESP_RETURN_ON_FALSE(xTaskCreate(servo_task, "servo", 3072, NULL, 3, NULL)==pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

esp_err_t servo_position(unsigned us)
{
    if (us < 1000 || us > 2000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    sweeping = false;
    esp_err_t err = pulse(us);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t servo_sweep_until(unsigned lo, unsigned hi, unsigned ms, int64_t deadline_us)
{
    if (lo < 1000 || hi > 2000 || lo >= hi || ms < 400 || ms > 60000)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = pulse(lo);
    if (err == ESP_OK) {
        low=lo; high=hi; period=ms; start=esp_timer_get_time(); stop_at=deadline_us; sweeping=true;
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t servo_sweep(unsigned lo, unsigned hi, unsigned ms)
{
    return servo_sweep_until(lo,hi,ms,0);
}

void servo_stop(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    sweeping=false;
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    xSemaphoreGive(lock);
}

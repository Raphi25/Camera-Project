/* Generate once and persist per-device media/log encryption credentials. */

#include "device_credentials.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CREDENTIAL_NAMESPACE "credentials"
#define MEDIA_KEY "media_key"
#define LOG_KEY   "log_key"
#define SECRET_BYTES 32
#define SECRET_TEXT_SIZE (SECRET_BYTES * 2 + 1)

static char s_media_password[SECRET_TEXT_SIZE];
static char s_log_password[SECRET_TEXT_SIZE];
static bool s_ready;

static esp_err_t load_or_create(nvs_handle_t handle, const char *key, char *out)
{
    size_t size = SECRET_TEXT_SIZE;
    esp_err_t err = nvs_get_str(handle, key, out, &size);
    if (err == ESP_OK && size == SECRET_TEXT_SIZE) return ESP_OK;
    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH) return err;

    uint8_t random[SECRET_BYTES];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) {
        snprintf(&out[i * 2], 3, "%02X", random[i]);
    }
    out[SECRET_TEXT_SIZE - 1] = '\0';
    return nvs_set_str(handle, key, out);
}

esp_err_t device_credentials_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CREDENTIAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = load_or_create(handle, MEDIA_KEY, s_media_password);
    if (err == ESP_OK) err = load_or_create(handle, LOG_KEY, s_log_password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    s_ready = err == ESP_OK;
    return err;
}

const char *device_credentials_media_password(void)
{
    return s_ready ? s_media_password : NULL;
}

const char *device_credentials_log_password(void)
{
    return s_ready ? s_log_password : NULL;
}

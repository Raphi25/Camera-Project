/* Append-only authenticated log records with atomic initialization and recovery. */

#include "log_sink.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#include "crypto_kdf.h"
#include "sd_storage.h"

#define LOG_SINK_MAGIC             0x31474F4Cu
#define LOG_SINK_VERSION           1u
#define LOG_SINK_SALT_LEN          16u
#define LOG_SINK_NONCE_LEN         12u
#define LOG_SINK_KEY_LEN           32u
#define LOG_SINK_TAG_LEN           16u
#define LOG_SINK_PBKDF2_ITERS      10000u
#define LOG_SINK_MAX_PLAINTEXT_LEN 256u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t iterations;
    uint8_t salt[LOG_SINK_SALT_LEN];
} log_sink_file_header_t;

typedef struct __attribute__((packed)) {
    uint16_t plain_len;
    uint8_t nonce[LOG_SINK_NONCE_LEN];
} log_sink_record_header_t;

static const char *TAG = "log_sink";
static char s_log_path[160] = {0};
static bool s_initialized = false;
static uint8_t s_key[LOG_SINK_KEY_LEN] = {0};
static mbedtls_svc_key_id_t s_key_id = MBEDTLS_SVC_KEY_ID_INIT;

static void reset_state(void)
{
    if (!mbedtls_svc_key_id_is_null(s_key_id)) {
        (void)psa_destroy_key(s_key_id);
        s_key_id = MBEDTLS_SVC_KEY_ID_INIT;
    }
    mbedtls_platform_zeroize(s_key, sizeof(s_key));
    s_initialized = false;
    s_log_path[0] = '\0';
}

static esp_err_t fill_random_bytes(uint8_t *buffer, size_t length)
{
    if (!buffer || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* IDF seeds its hardware RNG during startup. This public API replaces the
     * legacy CTR_DRBG interface removed by IDF 6's Mbed TLS 4 migration. */
    esp_fill_random(buffer, length);
    return ESP_OK;
}

static esp_err_t derive_key(const char *password, const uint8_t *salt)
{
    esp_err_t derive_err = crypto_pbkdf2_hmac_sha256(
        (const uint8_t *)password, strlen(password), salt, LOG_SINK_SALT_LEN,
        LOG_SINK_PBKDF2_ITERS, s_key, sizeof(s_key));
    if (derive_err != ESP_OK) {
        mbedtls_platform_zeroize(s_key, sizeof(s_key));
        return ESP_FAIL;
    }

    (void)psa_crypto_init();
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, LOG_SINK_KEY_LEN * 8U);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_status_t status = psa_import_key(&attributes, s_key, sizeof(s_key), &s_key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        s_key_id = MBEDTLS_SVC_KEY_ID_INIT;
        mbedtls_platform_zeroize(s_key, sizeof(s_key));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t write_new_header(const char *path, const char *password, const uint8_t *salt)
{
    log_sink_file_header_t header = {
        .magic = LOG_SINK_MAGIC,
        .version = LOG_SINK_VERSION,
        .iterations = LOG_SINK_PBKDF2_ITERS,
    };
    memcpy(header.salt, salt, LOG_SINK_SALT_LEN);

    sd_storage_atomic_file_t atomic_file;
    if (sd_storage_atomic_begin(&atomic_file, path, "wb") != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create %s", path);
        return ESP_FAIL;
    }
    FILE *f = atomic_file.file;

    size_t written = fwrite(&header, sizeof(header), 1, f);
    if (written != 1) {
        sd_storage_atomic_abort(&atomic_file);
        ESP_LOGE(TAG, "Failed to write header to %s", path);
        return ESP_FAIL;
    }
    if (sd_storage_atomic_commit(&atomic_file) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write header to %s", path);
        return ESP_FAIL;
    }

    return derive_key(password, salt);
}

static esp_err_t load_or_create_file(const char *path, const char *password)
{
    log_sink_file_header_t header = {0};
    sd_storage_lock();
    FILE *f = fopen(path, "rb");
    if (!f) {
        sd_storage_unlock();
        uint8_t salt[LOG_SINK_SALT_LEN] = {0};
        if (fill_random_bytes(salt, sizeof(salt)) != ESP_OK) {
            return ESP_FAIL;
        }
        return write_new_header(path, password, salt);
    }

    size_t read_count = fread(&header, sizeof(header), 1, f);
    fclose(f);
    sd_storage_unlock();
    if (read_count != 1 || header.magic != LOG_SINK_MAGIC || header.version != LOG_SINK_VERSION) {
        uint8_t salt[LOG_SINK_SALT_LEN] = {0};
        if (fill_random_bytes(salt, sizeof(salt)) != ESP_OK) {
            return ESP_FAIL;
        }
        return write_new_header(path, password, salt);
    }

    return derive_key(password, header.salt);
}

esp_err_t log_sink_init(const char *path, const char *password)
{
    if (!path || !password || path[0] == '\0' || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    reset_state();

    if (snprintf(s_log_path, sizeof(s_log_path), "%s", path) >= (int)sizeof(s_log_path)) {
        reset_state();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = load_or_create_file(s_log_path, password);
    if (err != ESP_OK) {
        reset_state();
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t log_sink_append(const char *line)
{
    if (!s_initialized || !line) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t plain_len = strlen(line);
    if (plain_len == 0 || plain_len > LOG_SINK_MAX_PLAINTEXT_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t nonce[LOG_SINK_NONCE_LEN] = {0};
    if (fill_random_bytes(nonce, sizeof(nonce)) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t cipher_and_tag[LOG_SINK_MAX_PLAINTEXT_LEN + LOG_SINK_TAG_LEN] = {0};
    size_t cipher_and_tag_len = 0;
    psa_status_t status = psa_aead_encrypt(s_key_id, PSA_ALG_GCM,
                                           nonce, sizeof(nonce), NULL, 0,
                                           (const uint8_t *)line, plain_len,
                                           cipher_and_tag, sizeof(cipher_and_tag),
                                           &cipher_and_tag_len);
    if (status != PSA_SUCCESS || cipher_and_tag_len != plain_len + LOG_SINK_TAG_LEN) {
        ESP_LOGE(TAG, "Encryption failed (%d)", (int)status);
        return ESP_FAIL;
    }

    log_sink_record_header_t record_header = {
        .plain_len = (uint16_t)plain_len,
    };
    memcpy(record_header.nonce, nonce, LOG_SINK_NONCE_LEN);

    sd_storage_lock();
    FILE *f = fopen(s_log_path, "ab");
    if (!f) {
        sd_storage_unlock();
        ESP_LOGE(TAG, "Failed to open %s", s_log_path);
        return ESP_FAIL;
    }

    uint8_t record[sizeof(record_header) + LOG_SINK_MAX_PLAINTEXT_LEN + LOG_SINK_TAG_LEN];
    size_t record_len = 0;
    memcpy(record + record_len, &record_header, sizeof(record_header));
    record_len += sizeof(record_header);
    memcpy(record + record_len, cipher_and_tag, plain_len);
    record_len += plain_len;
    memcpy(record + record_len, cipher_and_tag + plain_len, LOG_SINK_TAG_LEN);
    record_len += LOG_SINK_TAG_LEN;

    esp_err_t err = fwrite(record, 1, record_len, f) == record_len ? ESP_OK : ESP_FAIL;

    if (err == ESP_OK && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
        err = ESP_FAIL;
    }
    fclose(f);
    sd_storage_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to append encrypted record to %s", s_log_path);
    }
    return err;
}

void log_sink_deinit(void)
{
    reset_state();
}

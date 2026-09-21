/* PBKDF2-HMAC-SHA256 using public PSA primitives available in ESP-IDF 6. */
#include "crypto_kdf.h"

#include <stdlib.h>
#include <string.h>

#include "psa/crypto.h"

#define SHA256_LEN 32U

esp_err_t crypto_pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                                    const uint8_t *salt, size_t salt_len,
                                    uint32_t iterations,
                                    uint8_t *output, size_t output_len)
{
    if (!password || password_len == 0 || !salt || salt_len == 0 ||
        iterations == 0 || !output || output_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)psa_crypto_init();

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, password_len * 8U);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = psa_import_key(&attributes, password, password_len, &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    uint8_t *initial = malloc(salt_len + 4U);
    if (!initial) {
        (void)psa_destroy_key(key_id);
        return ESP_ERR_NO_MEM;
    }

    memcpy(initial, salt, salt_len);
    esp_err_t result = ESP_OK;
    size_t produced = 0;

    for (uint32_t block = 1; produced < output_len; ++block) {
        initial[salt_len] = (uint8_t)(block >> 24);
        initial[salt_len + 1] = (uint8_t)(block >> 16);
        initial[salt_len + 2] = (uint8_t)(block >> 8);
        initial[salt_len + 3] = (uint8_t)block;
        uint8_t u[SHA256_LEN] = {0};
        uint8_t next_u[SHA256_LEN] = {0};
        uint8_t accumulator[SHA256_LEN] = {0};
        size_t mac_len = 0;

        status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                 initial, salt_len + 4U, u, sizeof(u), &mac_len);
        if (status != PSA_SUCCESS || mac_len != SHA256_LEN) {
            result = ESP_FAIL;
            break;
        }

        memcpy(accumulator, u, sizeof(accumulator));
        for (uint32_t iteration = 1; iteration < iterations; ++iteration) {
            status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                     u, sizeof(u), next_u, sizeof(next_u), &mac_len);
            if (status != PSA_SUCCESS || mac_len != SHA256_LEN) {
                result = ESP_FAIL;
                break;
            }
            memcpy(u, next_u, sizeof(u));
            for (size_t i = 0; i < SHA256_LEN; ++i) {
                accumulator[i] ^= u[i];
            }
        }

        memset(u, 0, sizeof(u));
        memset(next_u, 0, sizeof(next_u));
        if (result != ESP_OK) {
            memset(accumulator, 0, sizeof(accumulator));
            break;
        }

        size_t copy_len = output_len - produced;
        if (copy_len > SHA256_LEN) {
            copy_len = SHA256_LEN;
        }
        memcpy(output + produced, accumulator, copy_len);
        memset(accumulator, 0, sizeof(accumulator));
        produced += copy_len;
    }

    memset(initial, 0, salt_len + 4U);
    free(initial);
    (void)psa_destroy_key(key_id);
    if (result != ESP_OK) {
        memset(output, 0, output_len);
    }
    return result;
}

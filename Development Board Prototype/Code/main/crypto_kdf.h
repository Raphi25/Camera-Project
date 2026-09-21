/* Password-based key derivation implemented through the PSA Crypto API. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t crypto_pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                                    const uint8_t *salt, size_t salt_len,
                                    uint32_t iterations,
                                    uint8_t *output, size_t output_len);

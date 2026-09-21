/* AES-GCM image blob API; callers own returned buffers. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Image encryption helper.
 *
 * Blob format is self-describing: a small header identifies encrypted captures,
 * followed by AES-GCM ciphertext.  Legacy/plain files are still readable by
 * image_crypto_load_plain_from_file(), which is useful during development and
 * migration.
 *
 * Ownership: functions returning uint8_t** allocate with malloc(); caller frees.
 */

/* Encrypt a JPEG/PNG payload into a password-protected blob suitable for SD. */
esp_err_t image_crypto_encrypt_blob(const uint8_t *plain,
                                    size_t plain_len,
                                    const char *password,
                                    uint8_t **out_blob,
                                    size_t *out_blob_len);

/* Load a file and return plaintext image bytes. Encrypted blobs are decrypted;
 * non-blob files are returned as-is for backwards compatibility. */
esp_err_t image_crypto_load_plain_from_file(const char *path,
                                            const char *password,
                                            uint8_t **out_plain,
                                            size_t *out_plain_len);

/* Fast metadata path used by IMG_LIST so listing does not decrypt every image. */
esp_err_t image_crypto_get_plain_size_from_file(const char *path,
                                                const char *password,
                                                size_t *out_plain_len);

/* Authenticated image encryption/decryption with versioned on-disk headers. */

#include "image_crypto.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "psa/crypto.h"

#include "crypto_kdf.h"

#define IMAGE_CRYPTO_MAGIC        0x31474D49u
#define IMAGE_CRYPTO_VERSION      1u
#define IMAGE_CRYPTO_SALT_LEN     16u
#define IMAGE_CRYPTO_NONCE_LEN    12u
#define IMAGE_CRYPTO_TAG_LEN      16u
#define IMAGE_CRYPTO_KEY_LEN      32u
#define IMAGE_CRYPTO_PBKDF2_ITERS 10000u
#define IMAGE_CRYPTO_CACHE_MAGIC  0x494D474Bu
#define IMAGE_CRYPTO_CACHE_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t plain_len;
    uint8_t salt[IMAGE_CRYPTO_SALT_LEN];
    uint8_t nonce[IMAGE_CRYPTO_NONCE_LEN];
    uint8_t tag[IMAGE_CRYPTO_TAG_LEN];
} image_crypto_header_t;

/* Deep sleep resets normal RAM, but RTC slow memory remains powered.  Retain
 * one password-derived session key so the expensive PBKDF2 operation is paid
 * once per powered session instead of once per photograph.  Each image still
 * receives a fresh 96-bit GCM nonce, and its salt remains in the v1 header, so
 * existing PC-side decryption stays fully compatible. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t checksum;
    uint8_t password_digest[32];
    uint8_t salt[IMAGE_CRYPTO_SALT_LEN];
    uint8_t key[IMAGE_CRYPTO_KEY_LEN];
} image_crypto_rtc_cache_t;

RTC_DATA_ATTR static image_crypto_rtc_cache_t s_rtc_key_cache;

static uint32_t cache_checksum(const image_crypto_rtc_cache_t *cache)
{
    /* FNV-1a is only an accidental-corruption check; password verification and
     * media authentication are still provided by SHA-256/PBKDF2 and AES-GCM. */
    uint32_t value = 2166136261u;
    const uint8_t *regions[] = {
        (const uint8_t *)&cache->version,
        cache->password_digest,
        cache->salt,
        cache->key,
    };
    const size_t lengths[] = {
        sizeof(cache->version),
        sizeof(cache->password_digest),
        sizeof(cache->salt),
        sizeof(cache->key),
    };
    for (size_t region = 0; region < sizeof(regions) / sizeof(regions[0]); ++region) {
        for (size_t i = 0; i < lengths[region]; ++i) {
            value ^= regions[region][i];
            value *= 16777619u;
        }
    }
    return value;
}

static esp_err_t password_digest(const char *password, uint8_t digest[32])
{
    size_t digest_len = 0;
    (void)psa_crypto_init();
    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        (const uint8_t *)password,
        strlen(password),
        digest,
        32,
        &digest_len);
    return status == PSA_SUCCESS && digest_len == 32 ? ESP_OK : ESP_FAIL;
}

static void fill_random(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    for (size_t i = 0; i < len; i += sizeof(uint32_t)) {
        uint32_t r = esp_random();
        size_t n = (len - i) < sizeof(uint32_t) ? (len - i) : sizeof(uint32_t);
        memcpy(buf + i, &r, n);
    }
}

static esp_err_t derive_key(const char *password, const uint8_t *salt, uint8_t out_key[IMAGE_CRYPTO_KEY_LEN])
{
    if (!password || !salt || !out_key) {
        return ESP_ERR_INVALID_ARG;
    }

    return crypto_pbkdf2_hmac_sha256((const uint8_t *)password, strlen(password),
                                     salt, IMAGE_CRYPTO_SALT_LEN,
                                     IMAGE_CRYPTO_PBKDF2_ITERS,
                                     out_key, IMAGE_CRYPTO_KEY_LEN);
}

static esp_err_t get_session_key(const char *password,
                                 uint8_t salt[IMAGE_CRYPTO_SALT_LEN],
                                 uint8_t key[IMAGE_CRYPTO_KEY_LEN])
{
    uint8_t digest[32] = {0};
    esp_err_t err = password_digest(password, digest);
    if (err != ESP_OK) {
        return err;
    }

    bool cache_valid =
        s_rtc_key_cache.magic == IMAGE_CRYPTO_CACHE_MAGIC &&
        s_rtc_key_cache.version == IMAGE_CRYPTO_CACHE_VERSION &&
        s_rtc_key_cache.checksum == cache_checksum(&s_rtc_key_cache) &&
        memcmp(s_rtc_key_cache.password_digest, digest, sizeof(digest)) == 0;
    if (cache_valid) {
        memcpy(salt, s_rtc_key_cache.salt, IMAGE_CRYPTO_SALT_LEN);
        memcpy(key, s_rtc_key_cache.key, IMAGE_CRYPTO_KEY_LEN);
        memset(digest, 0, sizeof(digest));
        return ESP_OK;
    }

    fill_random(salt, IMAGE_CRYPTO_SALT_LEN);
    err = derive_key(password, salt, key);
    if (err == ESP_OK) {
        image_crypto_rtc_cache_t cache = {
            .magic = IMAGE_CRYPTO_CACHE_MAGIC,
            .version = IMAGE_CRYPTO_CACHE_VERSION,
        };
        memcpy(cache.password_digest, digest, sizeof(digest));
        memcpy(cache.salt, salt, IMAGE_CRYPTO_SALT_LEN);
        memcpy(cache.key, key, IMAGE_CRYPTO_KEY_LEN);
        cache.checksum = cache_checksum(&cache);
        s_rtc_key_cache = cache;
        memset(&cache, 0, sizeof(cache));
    }
    memset(digest, 0, sizeof(digest));
    return err;
}

static psa_status_t import_aes_key(const uint8_t key[IMAGE_CRYPTO_KEY_LEN],
                                   psa_key_usage_t usage,
                                   mbedtls_svc_key_id_t *key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, IMAGE_CRYPTO_KEY_LEN * 8U);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_status_t status = psa_import_key(&attributes, key, IMAGE_CRYPTO_KEY_LEN, key_id);
    psa_reset_key_attributes(&attributes);
    return status;
}

static bool has_valid_header(const uint8_t *blob, size_t blob_len, const image_crypto_header_t **out_hdr)
{
    if (!blob || blob_len < sizeof(image_crypto_header_t)) {
        return false;
    }

    const image_crypto_header_t *hdr = (const image_crypto_header_t *)blob;
    if (hdr->magic != IMAGE_CRYPTO_MAGIC || hdr->version != IMAGE_CRYPTO_VERSION) {
        return false;
    }
    if ((size_t)hdr->plain_len > (blob_len - sizeof(image_crypto_header_t))) {
        return false;
    }

    if (out_hdr) {
        *out_hdr = hdr;
    }
    return true;
}

esp_err_t image_crypto_encrypt_blob(const uint8_t *plain,
                                    size_t plain_len,
                                    const char *password,
                                    uint8_t **out_blob,
                                    size_t *out_blob_len)
{
    if (!plain || plain_len == 0 || !password || !out_blob || !out_blob_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (plain_len > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total_len = sizeof(image_crypto_header_t) + plain_len;
    /* PSA's one-shot AEAD output appends the tag. Reserve those bytes, then
     * move the tag into the existing version-1 header to preserve the file format. */
    uint8_t *blob = (uint8_t *)malloc(total_len + IMAGE_CRYPTO_TAG_LEN);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }

    image_crypto_header_t *hdr = (image_crypto_header_t *)blob;
    hdr->magic = IMAGE_CRYPTO_MAGIC;
    hdr->version = IMAGE_CRYPTO_VERSION;
    hdr->reserved = 0;
    hdr->plain_len = (uint32_t)plain_len;
    fill_random(hdr->nonce, sizeof(hdr->nonce));

    uint8_t key[IMAGE_CRYPTO_KEY_LEN] = {0};
    esp_err_t err = get_session_key(password, hdr->salt, key);
    if (err != ESP_OK) {
        free(blob);
        return err;
    }

    (void)psa_crypto_init();
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = import_aes_key(key, PSA_KEY_USAGE_ENCRYPT, &key_id);
    size_t output_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_aead_encrypt(key_id, PSA_ALG_GCM,
                                  hdr->nonce, IMAGE_CRYPTO_NONCE_LEN,
                                  NULL, 0, plain, plain_len,
                                  blob + sizeof(image_crypto_header_t),
                                  plain_len + IMAGE_CRYPTO_TAG_LEN, &output_len);
    }
    if (!mbedtls_svc_key_id_is_null(key_id)) (void)psa_destroy_key(key_id);
    memset(key, 0, sizeof(key));

    if (status != PSA_SUCCESS || output_len != plain_len + IMAGE_CRYPTO_TAG_LEN) {
        free(blob);
        return ESP_FAIL;
    }
    memcpy(hdr->tag, blob + total_len, IMAGE_CRYPTO_TAG_LEN);

    *out_blob = blob;
    *out_blob_len = total_len;
    return ESP_OK;
}

static esp_err_t read_file_all(const char *path, uint8_t **out_data, size_t *out_len)
{
    if (!path || !out_data || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    long file_len = ftell(f);
    if (file_len <= 0) {
        fclose(f);
        return ESP_FAIL;
    }

    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)file_len);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, (size_t)file_len, f);
    fclose(f);
    if (n != (size_t)file_len) {
        free(buf);
        return ESP_FAIL;
    }

    *out_data = buf;
    *out_len = (size_t)file_len;
    return ESP_OK;
}

esp_err_t image_crypto_load_plain_from_file(const char *path,
                                            const char *password,
                                            uint8_t **out_plain,
                                            size_t *out_plain_len)
{
    /* New captures are authenticated AES-GCM blobs. Plain legacy files remain
     * readable so firmware upgrades do not strand media already on the card. */
    if (!path || !password || !out_plain || !out_plain_len) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    esp_err_t err = read_file_all(path, &blob, &blob_len);
    if (err != ESP_OK) {
        return err;
    }

    const image_crypto_header_t *hdr = NULL;
    if (!has_valid_header(blob, blob_len, &hdr)) {
        *out_plain = blob;
        *out_plain_len = blob_len;
        return ESP_OK;
    }

    size_t cipher_len = blob_len - sizeof(image_crypto_header_t);
    if (cipher_len != (size_t)hdr->plain_len) {
        free(blob);
        return ESP_FAIL;
    }

    uint8_t *plain = (uint8_t *)malloc((size_t)hdr->plain_len);
    if (!plain) {
        free(blob);
        return ESP_ERR_NO_MEM;
    }

    uint8_t key[IMAGE_CRYPTO_KEY_LEN] = {0};
    err = derive_key(password, hdr->salt, key);
    if (err != ESP_OK) {
        free(blob);
        free(plain);
        return err;
    }

    size_t plain_len = (size_t)hdr->plain_len;
    size_t authenticated_len = cipher_len + IMAGE_CRYPTO_TAG_LEN;
    uint8_t *resized = realloc(blob, blob_len + IMAGE_CRYPTO_TAG_LEN);
    if (resized == NULL) {
        memset(key, 0, sizeof(key));
        free(blob);
        free(plain);
        return ESP_ERR_NO_MEM;
    }
    blob = resized;
    hdr = (const image_crypto_header_t *)blob;
    memcpy(blob + blob_len, hdr->tag, IMAGE_CRYPTO_TAG_LEN);

    (void)psa_crypto_init();
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t status = import_aes_key(key, PSA_KEY_USAGE_DECRYPT, &key_id);
    size_t output_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_aead_decrypt(key_id, PSA_ALG_GCM,
                                  hdr->nonce, IMAGE_CRYPTO_NONCE_LEN,
                                  NULL, 0,
                                  blob + sizeof(image_crypto_header_t), authenticated_len,
                                  plain, plain_len, &output_len);
    }
    if (!mbedtls_svc_key_id_is_null(key_id)) (void)psa_destroy_key(key_id);
    memset(key, 0, sizeof(key));
    free(blob);

    if (status != PSA_SUCCESS || output_len != plain_len) {
        free(plain);
        return ESP_FAIL;
    }

    *out_plain = plain;
    *out_plain_len = plain_len;
    return ESP_OK;
}

esp_err_t image_crypto_get_plain_size_from_file(const char *path,
                                                const char *password,
                                                size_t *out_plain_len)
{
    (void)password;
    if (!path || !out_plain_len) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    image_crypto_header_t hdr = {0};
    size_t read_n = fread(&hdr, 1, sizeof(hdr), f);
    if (read_n == sizeof(hdr) && hdr.magic == IMAGE_CRYPTO_MAGIC && hdr.version == IMAGE_CRYPTO_VERSION) {
        fclose(f);
        *out_plain_len = (size_t)hdr.plain_len;
        return ESP_OK;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    long file_len = ftell(f);
    fclose(f);
    if (file_len < 0) {
        return ESP_FAIL;
    }

    *out_plain_len = (size_t)file_len;
    return ESP_OK;
}

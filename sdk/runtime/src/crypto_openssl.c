/**
 * @file    crypto_openssl.c
 * @brief   Default Crypto vtable implementation — OpenSSL.
 *          A separate cmake target (thing_model_client_crypto_openssl);
 *          when you don't want to link OpenSSL, don't link this target and provide your own T_ThingModelCryptoVtable.
 */
#include "thing_model_client/thing_model_client_crypto.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

static int openssl_hmac_sha256(const uint8_t *key, size_t key_len,
                               const uint8_t *data, size_t data_len,
                               uint8_t out[32])
{
    unsigned int out_len = 32;
    if (!HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &out_len)) {
        return -1;
    }
    return (out_len == 32) ? 0 : -2;
}

static int openssl_random_bytes(uint8_t *out, size_t len)
{
    return (RAND_bytes(out, (int)len) == 1) ? 0 : -1;
}

static const T_ThingModelCryptoVtable g_openssl_vt = {
    .hmac_sha256  = openssl_hmac_sha256,
    .random_bytes = openssl_random_bytes,
};

const T_ThingModelCryptoVtable *ThingModelCrypto_OpenSSL(void)
{
    return &g_openssl_vt;
}

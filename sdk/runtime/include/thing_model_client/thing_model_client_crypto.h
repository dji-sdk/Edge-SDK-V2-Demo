/**
 * @file    thing_model_client_crypto.h
 * @brief   Crypto abstraction — collapses the runtime's dependency on "non-source libs" like OpenSSL into a single vtable layer.
 *
 * The runtime's default implementation is OpenSSL (crypto_openssl.c).
 * Users who don't want to link OpenSSL (embedded, having their own SHA256 implementation, using mbedtls/wolfssl):
 *
 *   1. Implement the T_ThingModelCryptoVtable below;
 *   2. Point .crypto_vtable to it in the Session config:
 *        cfg.crypto_vtable = &my_mbedtls_crypto;
 *   3. Don't link the thing_model_client_crypto_openssl target.
 *
 * Note: **TLS keying material export** (RFC5705) is not in this vtable —— it is bound to a specific TLS
 * connection, and lives in T_ThingModelTransportVtable.export_keying_material.
 */
#ifndef THING_MODEL_CLIENT_CRYPTO_H
#define THING_MODEL_CLIENT_CRYPTO_H

#include "thing_model_client_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct T_ThingModelCryptoVtable {
    /** HMAC-SHA256.
     *  @param key       key
     *  @param key_len   typically 32
     *  @param data      data to be signed
     *  @param data_len  data length
     *  @param out       output 32-byte MAC (caller guarantees capacity)
     *  @return 0 on success, < 0 on failure
     */
    int (*hmac_sha256)(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out[32]);

    /** Cryptographically secure random bytes (reserved — not currently used by the runtime, implementation may set NULL). */
    int (*random_bytes)(uint8_t *out, size_t len);
} T_ThingModelCryptoVtable;

/** Default OpenSSL implementation (in the thing_model_client_crypto_openssl cmake target).
 *  When you don't want to link OpenSSL: don't link this target, and give your own vtable in the Session config. */
const T_ThingModelCryptoVtable *ThingModelCrypto_OpenSSL(void);

/** Default mbedTLS implementation (in the thing_model_client_crypto_mbedtls cmake target).
 *  Embedded platforms (GD32 / FreeRTOS) prefer this implementation — same stack as the lwIP+mbedTLS transport.
 *  The caller is responsible for providing the mbedTLS config (usually -DMBEDTLS_CONFIG_FILE=...) and the entropy
 *  source (embedded enables MBEDTLS_ENTROPY_HARDWARE_ALT and implements mbedtls_hardware_poll). */
const T_ThingModelCryptoVtable *ThingModelCrypto_Mbedtls(void);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_CRYPTO_H */

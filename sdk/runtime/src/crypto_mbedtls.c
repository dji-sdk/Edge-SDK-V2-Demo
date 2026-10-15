/**
 * @file    crypto_mbedtls.c
 * @brief   Default Crypto vtable implementation — mbedTLS.
 *
 *   Standalone cmake target (thing_model_client_crypto_mbedtls), parallel to the OpenSSL implementation.
 *   Embedded platforms such as GD32 prefer this implementation.
 *
 *   Features:
 *     - hmac_sha256: mbedtls_md_hmac (SHA-256)
 *     - random_bytes: mbedtls_ctr_drbg_random
 *         entropy is collected via mbedTLS's internal mbedtls_entropy_func;
 *         embedded platforms enable MBEDTLS_ENTROPY_HARDWARE_ALT and provide mbedtls_hardware_poll
 *         (this repo's samples/sample_things_model_rtos_gd32/network/tls/mbedtls_hw_rng.c
 *         uses the GD32 TRNG).
 *
 *   Threading model:
 *     - The DRBG/entropy context is module-local + lazy init, initialized on the first random_bytes call.
 *     - When multiple threads call random_bytes concurrently, the DRBG's internal mutex is provided by mbedTLS
 *       MBEDTLS_THREADING_C; if THREADING_C is not enabled, this file provides an OSAL mutex as a fallback.
 *     - hmac_sha256 is a stateless call, inherently thread-safe (the mbedTLS API itself assumes the caller holds the lock).
 *
 *   The mbedTLS config file is provided by the build environment (embedded usually -DMBEDTLS_CONFIG_FILE=...);
 *   this file does not bind a specific config, it only uses public APIs.
 */
#include "thing_model_client/thing_model_client_crypto.h"
#include "thing_model_client/thing_model_client_osal.h"

#include "mbedtls/md.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include <string.h>

/* ============================================================
 * HMAC-SHA256
 * ============================================================ */
static int mbedtls_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                       const uint8_t *data, size_t data_len,
                                       uint8_t out[32])
{
    if (!key || !data || !out) return -1;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    int ret = mbedtls_md_hmac(md, key, key_len, data, data_len, out);
    return (ret == 0) ? 0 : -1;
}

/* ============================================================
 * Random bytes — CTR_DRBG seeded by entropy_func (hardware_poll on GD32)
 *
 * lazy init: seed on the first random_bytes, then reuse directly afterwards.
 * Locking: this file uses an OSAL mutex to protect the drbg context (simple, assume single SDK instance).
 *      Does not depend on mbedTLS THREADING_C — reduces configuration coupling.
 * ============================================================ */
static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static int                      s_drbg_inited = 0;
static void                    *s_drbg_mutex  = NULL;  /* lazy create */

static int ensure_drbg_inited(void)
{
    /* Double-checked locking: most calls take the fast path */
    if (s_drbg_inited) return 0;

    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (!o) return -1;  /* OSAL not registered → caller timing error */

    if (!s_drbg_mutex) {
        /* Note: there is a window where multiple threads race into init simultaneously; here it is avoided with
         *    the weak assumption "the first random_bytes is called once during single-threaded startup". session Connect
         *    does not directly call random_bytes internally (EKM goes through transport_lwip), so the init
         *    timing is guaranteed by the user/upper layer. The lazy init assumes the first random_bytes call
         *    happens during single-threaded startup. */
        s_drbg_mutex = o->mutex_create();
        if (!s_drbg_mutex) return -1;
    }

    o->mutex_lock(s_drbg_mutex);
    if (!s_drbg_inited) {
        mbedtls_entropy_init(&s_entropy);
        mbedtls_ctr_drbg_init(&s_drbg);

        static const char *pers = "thing_model_client_drbg";
        int ret = mbedtls_ctr_drbg_seed(&s_drbg,
                                         mbedtls_entropy_func, &s_entropy,
                                         (const unsigned char *)pers,
                                         strlen(pers));
        if (ret == 0) {
            s_drbg_inited = 1;
        } else {
            mbedtls_ctr_drbg_free(&s_drbg);
            mbedtls_entropy_free(&s_entropy);
        }
    }
    o->mutex_unlock(s_drbg_mutex);
    return s_drbg_inited ? 0 : -1;
}

static int mbedtls_crypto_random_bytes(uint8_t *out, size_t len)
{
    if (!out) return -1;
    if (ensure_drbg_inited() != 0) return -1;

    const T_ThingModelOsal *o = ThingModelOsal_Get();
    o->mutex_lock(s_drbg_mutex);
    int ret = mbedtls_ctr_drbg_random(&s_drbg, out, len);
    o->mutex_unlock(s_drbg_mutex);
    return (ret == 0) ? 0 : -1;
}

/* ============================================================
 * vtable + factory
 * ============================================================ */
static const T_ThingModelCryptoVtable g_mbedtls_vt = {
    .hmac_sha256  = mbedtls_crypto_hmac_sha256,
    .random_bytes = mbedtls_crypto_random_bytes,
};

const T_ThingModelCryptoVtable *ThingModelCrypto_Mbedtls(void)
{
    return &g_mbedtls_vt;
}

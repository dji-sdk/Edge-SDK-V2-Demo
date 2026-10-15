/**
 * @file    thing_model_client_secure_access.c
 * @brief   auth token computation + payload HMAC signing.
 *          Only calls the crypto vtable (HMAC-SHA256); does not directly #include any third-party crypto library.
 *          TLS keying material export is handled by transport vtable.export_keying_material.
 */
#include "thing_model_client_secure_access.h"
#include <string.h>
#include <stdio.h>

#define SA_KEY_LEN        32
#define SA_HMAC_LEN       32
#define SA_TS_LEN         8

int ThingModelSA_ComputeAuthToken(const T_ThingModelCryptoVtable *crypto,
                                  const uint8_t hmac_key[32], const char *device_sn,
                                  char *out_token, size_t out_cap)
{
    if (!crypto || !crypto->hmac_sha256) return -1;
    if (!hmac_key || !device_sn || !out_token || out_cap < (6 + 2*SA_HMAC_LEN + 1))
        return -1;

    uint8_t mac[SA_HMAC_LEN];
    if (crypto->hmac_sha256(hmac_key, SA_KEY_LEN,
                            (const uint8_t *)device_sn, strlen(device_sn),
                            mac) != 0) {
        return -2;
    }
    memcpy(out_token, "token:", 6);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < SA_HMAC_LEN; i++) {
        out_token[6 + 2*i]     = hex[mac[i] >> 4];
        out_token[6 + 2*i + 1] = hex[mac[i] & 0x0f];
    }
    out_token[6 + 2*SA_HMAC_LEN] = '\0';
    return 0;
}

static void encode_u64_be(uint64_t v, uint8_t out[8])
{
    for (int i = 0; i < 8; i++) out[7 - i] = (uint8_t)((v >> (8*i)) & 0xff);
}

int ThingModelSA_SignPayload(const T_ThingModelCryptoVtable *crypto,
                             const uint8_t hmac_key[32],
                             const void *payload, size_t payload_len,
                             uint64_t ts_ms,
                             uint8_t *out_buf, size_t out_cap, size_t *out_len)
{
    if (!crypto || !crypto->hmac_sha256) return -1;
    if (!hmac_key || !payload || !out_buf || !out_len) return -1;
    if (out_cap < payload_len + SA_TS_LEN + SA_HMAC_LEN) return -2;

    /* copy payload + ts */
    memcpy(out_buf, payload, payload_len);
    encode_u64_be(ts_ms, out_buf + payload_len);

    /* hmac over payload+ts */
    uint8_t mac[SA_HMAC_LEN];
    if (crypto->hmac_sha256(hmac_key, SA_KEY_LEN,
                            out_buf, payload_len + SA_TS_LEN,
                            mac) != 0) {
        return -3;
    }
    memcpy(out_buf + payload_len + SA_TS_LEN, mac, SA_HMAC_LEN);
    *out_len = payload_len + SA_TS_LEN + SA_HMAC_LEN;
    return 0;
}

#ifndef THING_MODEL_CLIENT_SECURE_ACCESS_H
#define THING_MODEL_CLIENT_SECURE_ACCESS_H

#include <stdint.h>
#include <stddef.h>
#include "thing_model_client/thing_model_client_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compute the auth token: "token:" + hex(HMAC-SHA256(hmac_key, device_sn)).
 *  @param crypto  crypto vtable (NULL not allowed)
 *  @param hmac_key  32-byte keying material
 *  @param device_sn  device serial number
 *  @param out_token  output buffer, capacity must be >= 71
 *  @param out_cap    out_token capacity
 *  @return 0 on success */
int ThingModelSA_ComputeAuthToken(const T_ThingModelCryptoVtable *crypto,
                                  const uint8_t hmac_key[32],
                                  const char *device_sn,
                                  char *out_token, size_t out_cap);

/** Sign a payload: output = payload || ts_be8 || HMAC32.
 *  @param crypto  crypto vtable (NULL not allowed)
 *  @param hmac_key  32-byte keying material
 *  @param payload   data to be signed
 *  @param payload_len
 *  @param ts_ms     timestamp (milliseconds)
 *  @param out_buf   output
 *  @param out_cap   out_buf capacity, must be >= payload_len + 40
 *  @param out_len   number of bytes written
 *  @return 0 on success */
int ThingModelSA_SignPayload(const T_ThingModelCryptoVtable *crypto,
                             const uint8_t hmac_key[32],
                             const void *payload, size_t payload_len,
                             uint64_t ts_ms,
                             uint8_t *out_buf, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_SECURE_ACCESS_H */

/**
 * @file    thing_model_client_transport.h
 * @brief   MQTT transport abstraction.
 */
#ifndef THING_MODEL_CLIENT_TRANSPORT_H
#define THING_MODEL_CLIENT_TRANSPORT_H

#include "thing_model_client_types.h"
#include "thing_model_client_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct T_ThingModelTransportImpl T_ThingModelTransportImpl_t;

typedef void (*T_ThingModelTransportMsgCb)(void *user, const char *topic,
                                    const void *payload, size_t payload_len);
typedef void (*T_ThingModelTransportConnCb)(void *user, int rc);

typedef struct {
    const char *host;
    uint16_t    port;
    bool        use_tls;
    const char *ca_cert_path;
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t    keepalive_sec;
    bool        clean_session;
    bool        hmac_enabled;
    const uint8_t *hmac_key;
    /** crypto vtable used for HMAC; must be non-NULL when hmac_enabled=true */
    const T_ThingModelCryptoVtable *crypto;
    /** Auto-reconnect — the libmosquitto background thread reconnects per the backoff strategy; disabled when false */
    bool        auto_reconnect;
    uint16_t    reconnect_delay_min_sec;   /* 0 = use default 1s */
    uint16_t    reconnect_delay_max_sec;   /* 0 = use default 30s */
} T_ThingModelTransportConfig;

typedef struct T_ThingModelTransportVtable {
    T_ThingModelTransportImpl_t *(*create)(void);
    void                  (*destroy)(T_ThingModelTransportImpl_t *);

    void (*set_message_cb)(T_ThingModelTransportImpl_t *, T_ThingModelTransportMsgCb cb, void *user);
    void (*set_conn_cb)   (T_ThingModelTransportImpl_t *, T_ThingModelTransportConnCb cb, void *user);

    T_ThingModelReturnCode (*connect)   (T_ThingModelTransportImpl_t *, const T_ThingModelTransportConfig *cfg);
    T_ThingModelReturnCode (*disconnect)(T_ThingModelTransportImpl_t *);

    T_ThingModelReturnCode (*publish)   (T_ThingModelTransportImpl_t *, const char *topic,
                                  const void *payload, size_t len, int qos, bool retain);
    T_ThingModelReturnCode (*subscribe) (T_ThingModelTransportImpl_t *, const char *topic_filter, int qos);
    T_ThingModelReturnCode (*unsubscribe)(T_ThingModelTransportImpl_t *, const char *topic_filter);

    T_ThingModelReturnCode (*export_keying_material)(T_ThingModelTransportImpl_t *, uint8_t out[32]);
} T_ThingModelTransportVtable;

const T_ThingModelTransportVtable *ThingModelTransport_Mosquitto(void);

/** lwIP + altcp_tls + mbedTLS implementation (in the thing_model_client_transport_lwip cmake target).
 *  Used by embedded platforms (GD32 / FreeRTOS + lwIP NO_SYS=0 + LWIP_TCPIP_CORE_LOCKING=1).
 *
 *  Usage constraints:
 *    1. Must first call ThingModelOsal_RegisterFreeRtos();
 *    2. cfg.crypto_vtable is recommended to use ThingModelCrypto_Mbedtls();
 *    3. The CA cert is returned by the sample project implementing the two weak functions below:
 *         extern const unsigned char *thing_model_lwip_ca_cert(void);
 *         extern unsigned int          thing_model_lwip_ca_cert_len(void);
 *       (the cfg->ca_cert_path field is ignored under this implementation, this pair of functions takes precedence — embedded has no FS)
 *    4. lwIP must be configured with NO_SYS=0 + LWIP_TCPIP_CORE_LOCKING=1; the transport internally
 *       LOCK_TCPIP_CORE / UNLOCK_TCPIP_CORE automatically around mqtt_* API calls, so an application thread
 *       can call publish/subscribe at any time (consistent with transport_mosq.c behavior).
 */
const T_ThingModelTransportVtable *ThingModelTransport_LwipMbedtls(void);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_TRANSPORT_H */

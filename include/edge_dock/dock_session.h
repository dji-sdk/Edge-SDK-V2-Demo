/**
 ********************************************************************
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * dock_session —— connection lifecycle wrapper.
 *   UDP-discover broker → assemble ClientSession_Config → Connect → provide session handle.
 ********************************************************************
 */
#ifndef DOCK_SESSION_H
#define DOCK_SESSION_H

#include <stdbool.h>
#include "thing_model_client/thing_model_client_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Connection parameters. When empty, the default macros in dock_app_info.h are used. */
    const char *app_id;
    const char *app_key;
    const char *app_license;
    const char *app_name;
    /* Raw third-party device SN. Set for you by dock_session_default_opts() (dt5, DOCK_DEVICE_SN)
     * or dock_session_default_opts_rid() (dt7, DOCK_RID_DEVICE_SN) — assign by hand only to override. */
    const char *device_sn;
    const char *broker_ip;      /* if non-empty, skip UDP discovery and use this directly */
    int         broker_port;

    /* UDP discovery: if true, listen on 18830 first; on failure fall back to fallback_ip. */
    bool        use_discovery;
    int         discovery_timeout_ms;
    const char *fallback_ip;

    /* ESDK 2.0 plaintext access (default true): single-shot plaintext MQTT, username=app_id"_esdk_"sn.
     * If false, use the runtime two-phase TLS+HMAC secure-access (requires a CA, a different kind of broker). */
    bool        plaintext_access;
    /* Plaintext-access username suffix; RID devices use "@rid", which raises the single-packet
     * limit that RID/ADS-B telemetry needs. Set for you by dock_session_default_opts_rid() —
     * don't hand-set it for dt7.
     * Can be overridden by the DOCK_USERNAME_SUFFIX environment variable. NULL/"" = no suffix. */
    const char *username_suffix;

    /* Optional: inject a custom transport (e.g. a loopback mock in CI). NULL=default mosquitto.
     * Type is const T_ThingModelTransportVtable * (uses void* to avoid spreading header dependencies). */
    const void *transport_vtable;

    /* Re-send capability_report after every silent auto-reconnect. Mandatory, not an optimisation.
     *
     * The gateway keeps a third-party device registered only for the life of ONE MQTT session.
     * auto_reconnect is on, so the SDK restores the link by itself — but the device comes back
     * CONNECTED AND UNREGISTERED: its peripheral stays at status:2, and the gateway then silently
     * discards every point-to-point requests_reply (the sdr / network / charging domains, answered
     * by third_dock_server instead of published straight to the standard reply topic). Nothing
     * reports an error; those domains just time out while tsl/cloud/org_bind keep working.
     *
     * Filled in for you: dock_session_default_opts()     -> auth_report_capability_dt5
     *                    dock_session_default_opts_rid() -> auth_report_capability_dt7
     * Set to NULL to opt out. Always go through those helpers: an uninitialised DockSessionOpts now
     * carries a wild function pointer, not just a wild string.
     *
     * Runs on the runtime's reconnect worker thread ("tm_reconn"), which also owns handling the NEXT
     * drop, so the hook must be short and non-blocking. In particular do NOT plug in
     * auth_report_capability_dt5_sync(): it parks that thread for up to timeout_ms waiting for a
     * state_reply that nothing subscribes to on a real dock.
     *
     * The FIRST connect is NOT covered — report it yourself right after dock_session_connect()
     * returns, once your listeners and subscriptions are in place (see samples/). */
    int (*on_reconnect)(ThingModelClientSession_t *session);
} DockSessionOpts;

/** Fill opts with the dt5 (third-party dock) defaults from dock_app_info.h.
 *  device_sn = DOCK_DEVICE_SN, overridable at run time by the DOCK_DEVICE_SN environment variable. */
void dock_session_default_opts(DockSessionOpts *opts);

/**
 * Fill opts with the dt7 (RID) device identity: same credentials/broker settings as
 * dock_session_default_opts(), but device_sn = DOCK_RID_DEVICE_SN and username_suffix = "@rid".
 *
 * dt5 and dt7 are two DISTINCT devices. The account "<app_id>_esdk_<device_sn>" is both the MQTT
 * client_id and the topic owner segment, so they must not share an SN: whichever connects SECOND is
 * refused with CONNACK rc=5 (not authorised).
 *
 * Run-time override: the DOCK_RID_SN environment variable. Note DOCK_RID_SN and DOCK_DEVICE_SN are
 * independent: setting DOCK_DEVICE_SN=foo does NOT make dt7 become "foo_rid" (the "_rid" derivation
 * happens at CMake configure time only) — explicit beats derived.
 */
void dock_session_default_opts_rid(DockSessionOpts *opts);

/**
 * Connect to the O4 broker. On success returns a session handle, on failure returns NULL.
 * Internally: dock_id_provider_register → (optional) UDP discovery → Connect.
 *
 * Must be paired with dock_session_disconnect(): that is what releases the id provider and the
 * opts.on_reconnect hook slot. Tearing the session down with ThingModelClientSession_Disconnect()
 * directly leaks both.
 */
ThingModelClientSession_t *dock_session_connect(const DockSessionOpts *opts);

/** Disconnect, release the reconnect hook slot, and unregister the id provider. */
void dock_session_disconnect(ThingModelClientSession_t *session);

/**
 * Exactly what dock_session_connect() would hand the broker, materialised WITHOUT connecting.
 *
 * The account and password formulas live inside the runtime's auth path, where nothing outside
 * can see them; anyone needing the real credentials (to drive a raw MQTT client, to paste into
 * mosquitto_sub, to diagnose a CONNACK refusal) would otherwise re-derive them by hand and drift
 * the moment the runtime changes. This resolves them once, applying the same environment
 * precedence connect() applies (DOCK_BROKER_IP / DOCK_BROKER_PORT / DOCK_USERNAME_SUFFIX), and
 * running the same optional UDP discovery to fill broker_ip.
 *
 * Returns 0 on success, THING_MODEL_RC_INVALID_ARG on a NULL argument.
 */
typedef struct {
    char account [80];    /* = MQTT client_id = topic owner segment: app_id "_esdk_" device_sn */
    char username[96];    /* account + username_suffix ("@rid" for dt7)                        */
    char password[640];   /* app_key "_" app_license -- the license alone runs to ~350 bytes   */
    char broker_ip[64];
    int  broker_port;
} DockSessionCreds_t;

int dock_session_resolve_credentials(const DockSessionOpts *opts, DockSessionCreds_t *out);

/** The account = MQTT username = topic owner segment (e.g. "195_esdk_mqtt_test" = app_id"_esdk_"device_sn).
 *  This is NOT the raw device_sn: after connecting, everything that builds a topic or feeds a capability
 *  object (e.g. flight_create) must use this account, not opts.device_sn. Thin truthful alias for the
 *  runtime's GetSn(), whose name misleadingly reads as a bare SN. */
static inline const char *dock_session_account(ThingModelClientSession_t *session)
{
    return ThingModelClientSession_GetSn(session);
}

#ifdef __cplusplus
}
#endif

#endif /* DOCK_SESSION_H */

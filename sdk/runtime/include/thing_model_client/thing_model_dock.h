/**
 * @file    thing_model_dock.h
 * @brief   Third-party dock device — bid/tid injection primitive interface.
 *
 * Dock envelope = `{bid, tid, data, method, version, timestamp}` (different from Things-Model's `{seq,method,data}`).
 *
 *   - bid : business id, a business-defined capability / sub-capability id
 *   - tid : tracking id, a cross-endpoint tracking id for one task (UUIDv4 string etc.); one-to-one with reply
 *   - version : method protocol version (e.g. "1.0.0")
 *   - timestamp : Unix epoch milliseconds, via OSAL time_ms()
 *
 * [Architecture] The envelope encode/parse is **already inlined into the generated code** (see
 * `<Pkg>Dock_EncodeRequest/EncodeReply/ParseEnvelope` in <cap>_json.{h,c}), symmetric to how tm-uav's seq envelope is assembled in the generated code.
 * This header exposes only the bid/tid injection primitives — you inject a generator, and MakeBid/MakeTid are called to obtain values.
 *
 * Authentication (bidirectional) uses the Things-Model two-phase authentication.
 */
#ifndef THING_MODEL_DOCK_H
#define THING_MODEL_DOCK_H

#include "thing_model_client_types.h"
#include "thing_model_client_codec.h"
#include "thing_model_client_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * bid / tid injection (process-level singleton, same pattern as OSAL)
 * ============================================================ */

/**
 * Dock envelope bid/tid generator —— injected by business code.
 *
 *   gen_bid(user, method): decides the bid for this method call; returns a heap string that is freed via ThingModelMem_Free when done
 *                          (business allocates via ThingModelMem_Strdup/Alloc). May return a different bid per method.
 *                          Returning NULL is allowed → bid="" placeholder on the wire.
 *   gen_tid(user): decides the tid for this request; same as above, a heap string. Expected to be a globally unique string like a UUIDv4.
 *
 * When not registered, an empty string is used as placeholder.
 */
typedef struct {
    char *(*gen_bid)(void *user, const char *method);
    char *(*gen_tid)(void *user);
    void  *user;
} T_ThingModelDockIdProvider;

void ThingModelDockIdProvider_Register(const T_ThingModelDockIdProvider *p);
const T_ThingModelDockIdProvider *ThingModelDockIdProvider_Get(void);

/**
 * Injection primitives: obtain bid / tid (called by the envelope assembly, not usually by application code).
 * Returns a heap string, the caller calls ThingModelMem_Free when done; not registered → returns "" (empty-string placeholder).
 */
char *ThingModelDockIdProvider_MakeBid(const char *method);
char *ThingModelDockIdProvider_MakeTid(void);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_DOCK_H */

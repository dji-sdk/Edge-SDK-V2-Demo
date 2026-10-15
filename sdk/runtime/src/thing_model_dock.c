/**
 * @file    thing_model_dock.c
 * @brief   Dock device bid/tid injection primitives (process-level singleton).
 *
 * The envelope (encode/parse) is already inlined into the generated code (see templates/c/tm-dock/envelope.{c,h}.j2),
 * symmetric to how tm-uav's seq envelope is assembled in the generated code. The runtime keeps only the **injection primitives**:
 * the business side provides bid/tid generators, and the generated code calls MakeBid/MakeTid to obtain values and assemble the envelope itself.
 */
#include "thing_model_client/thing_model_dock.h"

#include <string.h>

/* ============================================================
 * bid / tid injection (process-level singleton)
 * ============================================================ */

static T_ThingModelDockIdProvider s_id_provider = {0};
static bool                       s_id_provider_set = false;

void ThingModelDockIdProvider_Register(const T_ThingModelDockIdProvider *p)
{
    if (!p) {
        /* Reset — allows the business to clean up after Disconnect */
        memset(&s_id_provider, 0, sizeof(s_id_provider));
        s_id_provider_set = false;
        return;
    }
    s_id_provider     = *p;
    s_id_provider_set = true;
}

const T_ThingModelDockIdProvider *ThingModelDockIdProvider_Get(void)
{
    return s_id_provider_set ? &s_id_provider : NULL;
}

/* Get bid / tid: returns a heap string, the caller (the generated-code envelope) calls ThingModelMem_Free when done.
 * When the IdProvider is not registered or the callback returns NULL → returns Strdup("") as a fallback (an empty string appears
 * on the wire, so during business troubleshooting "forgot to register" is spotted at a glance). */
char *ThingModelDockIdProvider_MakeBid(const char *method)
{
    if (s_id_provider_set && s_id_provider.gen_bid) {
        char *b = s_id_provider.gen_bid(s_id_provider.user, method);
        if (b) return b;
    }
    return ThingModelMem_Strdup("");
}

char *ThingModelDockIdProvider_MakeTid(void)
{
    if (s_id_provider_set && s_id_provider.gen_tid) {
        char *t = s_id_provider.gen_tid(s_id_provider.user);
        if (t) return t;
    }
    return ThingModelMem_Strdup("");
}

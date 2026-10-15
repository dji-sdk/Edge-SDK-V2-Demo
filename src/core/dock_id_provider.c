/**
 ********************************************************************
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * dock_id_provider implementation.
 ********************************************************************
 */
#include "edge_dock/dock_id_provider.h"

#include "thing_model_client/thing_model_dock.h"
#include "thing_model_client/thing_model_client_osal.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── transaction id pin (thread-local) ──────────────────────────────────────────────────────────
 * A progress event has to go out under the bid/tid of the service request that started it. The
 * envelope assembler only ever asks gen_bid/gen_tid, so this is where the echo has to happen.
 * Thread-local rather than global because surveillance's reporter thread publishes unsolicited
 * RID/ADSB events through this same provider and must keep getting fresh ids. Same TLS idiom the
 * runtime uses for its own per-thread last-error slot. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define DOCK_TLS _Thread_local
#else
#define DOCK_TLS __thread
#endif

/* 128 (DOCK_ID_MAX), not 37: the dock's tid is not contractually a UUID. */
static DOCK_TLS char s_pin_bid[DOCK_ID_MAX];
static DOCK_TLS char s_pin_tid[DOCK_ID_MAX];
static DOCK_TLS int  s_pinned;

#if defined(DOCK_HAVE_LIBUUID)
#include <uuid/uuid.h>
#else
/* Self-generate a UUID when libuuid is absent: entropy source for the extreme fallback (/dev/urandom unavailable). */
static char     s_sn[64]  = "dock";
static uint64_t s_counter = 0;

/* Fill n bytes from /dev/urandom; returns 1 on success, 0 on failure. */
static int fill_random(unsigned char *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}
#endif

/* Generate a heap UUID string (RFC-4122 v4 lowercase, 8-4-4-4-12); the caller (runtime) must Free it.
 * Same shape as the reference sample_esdk2 get_uuid(); works even without libuuid (e.g. cross-compile). */
static char *make_id(const char *prefix)
{
    (void)prefix;
#if defined(DOCK_HAVE_LIBUUID)
    uuid_t bin;
    char *s = (char *)ThingModelMem_Alloc(37);
    if (!s) return NULL;
    uuid_generate_random(bin);
    uuid_unparse_lower(bin, s);
    return s;
#else
    unsigned char b[16];
    if (!fill_random(b, sizeof(b))) {
        /* Extreme fallback when /dev/urandom is unavailable: seed xorshift64 with time_ms ^ counter ^ sn,
         * still producing a UUID shape (uniqueness within a single process is sufficient). */
        const T_ThingModelOsal *o = ThingModelOsal_Get();
        uint64_t seed = (o && o->time_ms) ? (uint64_t)o->time_ms() : 0;
        seed ^= (uint64_t)(++s_counter) << 21;
        for (const char *p = s_sn; *p; ++p)
            seed = seed * 1099511628211ull ^ (unsigned char)*p;
        for (int i = 0; i < 16; ++i) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            b[i] = (unsigned char)(seed & 0xFF);
        }
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* variant 10xx */
    char *s = (char *)ThingModelMem_Alloc(37);
    if (!s) return NULL;
    snprintf(s, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
#endif
}

/* The envelope assembler frees whatever these return, so a pinned id must be handed back as a
 * fresh heap copy -- it cannot alias the TLS buffer. */
static char *gen_bid(void *user, const char *method)
{
    (void)user; (void)method;
    if (s_pinned) return ThingModelMem_Strdup(s_pin_bid);
    /* bid = business capability id; in the demo, generating a new one each time is fine. */
    return make_id("bid");
}

static char *gen_tid(void *user)
{
    (void)user;
    if (s_pinned) return ThingModelMem_Strdup(s_pin_tid);
    return make_id("tid");
}

void dock_id_provider_pin(const char *bid, const char *tid)
{
    /* All-or-nothing: a pinned bid next to a fresh random tid is a mismatched pair, which reads on
     * the wire as a protocol violation rather than as the missing-id bug it actually is. */
    if (!bid || !bid[0] || !tid || !tid[0] ||
        strlen(bid) >= sizeof(s_pin_bid) || strlen(tid) >= sizeof(s_pin_tid)) {
        s_pinned = 0;
        return;
    }
    snprintf(s_pin_bid, sizeof(s_pin_bid), "%s", bid);
    snprintf(s_pin_tid, sizeof(s_pin_tid), "%s", tid);
    s_pinned = 1;
}

void dock_id_provider_unpin(void)
{
    s_pinned = 0;
}

void dock_id_provider_register(const char *dock_sn)
{
#if !defined(DOCK_HAVE_LIBUUID)
    if (dock_sn && dock_sn[0]) {
        snprintf(s_sn, sizeof(s_sn), "%s", dock_sn);   /* used only for the extreme fallback entropy */
    }
#else
    (void)dock_sn;
#endif
    static T_ThingModelDockIdProvider prov;
    prov.gen_bid = gen_bid;
    prov.gen_tid = gen_tid;
    prov.user    = NULL;
    ThingModelDockIdProvider_Register(&prov);
}

void dock_id_provider_unregister(void)
{
    dock_id_provider_unpin();
    ThingModelDockIdProvider_Register(NULL);
}

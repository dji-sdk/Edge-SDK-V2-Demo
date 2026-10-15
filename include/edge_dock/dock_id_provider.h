/**
 ********************************************************************
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * dock_id_provider —— ESDK envelope bid/tid generator (must be implemented per dock guide §4).
 * Prefers libuuid to generate UUIDv4; falls back to "SN-counter" when libuuid is unavailable.
 ********************************************************************
 */
#ifndef DOCK_ID_PROVIDER_H
#define DOCK_ID_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Buffer size for a bid / tid, NUL included. A UUIDv4 needs 37, but the dock's ids are not
 *  contractually UUIDs, so callers keep headroom and refuse anything longer rather than truncate:
 *  a truncated tid is exactly the silent mismatch this whole mechanism exists to prevent. */
#define DOCK_ID_MAX 128

/** Register the bid/tid generator with the runtime. dock_sn is used as the fallback string prefix when libuuid is unavailable.
 *  Call once at process startup, before Connect. */
void dock_id_provider_register(const char *dock_sn);

/** Unregister (call before exit). Also drops the calling thread's pin, if any. */
void dock_id_provider_unregister(void);

/** Pin the bid/tid that gen_bid/gen_tid hand out, so an envelope carries the ids of an existing
 *  transaction instead of minting fresh ones. A progress event must echo the bid/tid of the service
 *  request that started it, or the dock discards it as "tid mismatch".
 *
 *  The pin is THREAD-LOCAL, and has to be: surveillance's periodic reporter publishes RID/ADSB events
 *  from its own thread through this same process-wide provider, and those are unsolicited -- a global
 *  pin would stamp them with a flight transaction's tid.
 *
 *  pin/unpin must bracket exactly ONE publish, on every exit path. A leaked pin silently attaches a
 *  stale transaction's ids to whatever that thread sends next.
 *
 *  Deliberately not filtered by method: gen_bid takes a method and gen_tid does not, and the envelope
 *  assembler passes both as arguments of one call (evaluation order unspecified), so a method filter
 *  could only ever half-apply -- yielding a mismatched bid/tid pair, which is worse than no filter.
 *
 *  A NULL or empty bid or tid pins nothing: half a pin is worse than none.
 */
void dock_id_provider_pin(const char *bid, const char *tid);

/** Drop the calling thread's pin; later envelopes go back to fresh random ids. Safe when unpinned. */
void dock_id_provider_unpin(void);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_ID_PROVIDER_H */

/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Diagnostics part 1: the decode tables. Pure functions -- no session, no allocation, no I/O.
 *
 * Split out from diag.c on purpose: everything here is testable without a broker, and an
 * integrator who only wants to turn a number from a log into a sentence pays for nothing else.
 */
#include "edge_dock/diag.h"

#include <stdio.h>
#include <string.h>

/* The gateway's codes are 0x0101_<category>_<index>. Recognising the prefix lets a code that is
 * added on the gateway side later still decode as "unknown <class>" instead of a bare number. */
#define DIAG_CODE_PREFIX      0x01010000L
#define DIAG_CODE_PREFIX_MASK 0xFFFF0000L
#define DIAG_CAT_TOPIC_ACL    0x01
#define DIAG_CAT_QUOTA        0x02

static long diag_category(int64_t code)
{
    if (((long)code & DIAG_CODE_PREFIX_MASK) != DIAG_CODE_PREFIX) return -1;
    return ((long)code >> 8) & 0xFF;
}

DockDiagClass_t dock_diag_error_class(int64_t error_code)
{
    switch (diag_category(error_code)) {
        case DIAG_CAT_TOPIC_ACL: return DOCK_DIAG_CLASS_TOPIC_ACL;
        case DIAG_CAT_QUOTA:     return DOCK_DIAG_CLASS_QUOTA;
        default:                 return DOCK_DIAG_CLASS_UNKNOWN;
    }
}

const char *dock_diag_error_str(int64_t error_code)
{
    switch (error_code) {
        case DOCK_DIAG_ERR_UNKNOWN_AUTH:       return "unknown authentication failure";
        case DOCK_DIAG_ERR_TOPIC_MISMATCH:     return "topic does not match the required rules";
        case DOCK_DIAG_ERR_CLIENT_ID_MISMATCH: return "client id in the topic does not match the account";
        case DOCK_DIAG_ERR_SUB_NOT_ALLOWED:    return "subscribing to this topic is not allowed";
        case DOCK_DIAG_ERR_PUB_NOT_ALLOWED:    return "publishing to this topic is not allowed";
        case DOCK_DIAG_ERR_QOS_MISMATCH:       return "QoS does not match what this topic requires";
        case DOCK_DIAG_ERR_RATE_LIMIT:         return "message rate limit exceeded";
        case DOCK_DIAG_ERR_PAYLOAD_TOO_LARGE:  return "message length limit exceeded";
        default: break;
    }
    /* Not in the table: say as much as the code layout allows rather than pretending to know. */
    switch (dock_diag_error_class(error_code)) {
        case DOCK_DIAG_CLASS_TOPIC_ACL: return "unrecognised topic/ACL error code";
        case DOCK_DIAG_CLASS_QUOTA:     return "unrecognised quota error code";
        default:                        return "unrecognised error code";
    }
}

const char *dock_diag_error_hint(int64_t error_code)
{
    switch (error_code) {
        case DOCK_DIAG_ERR_UNKNOWN_AUTH:
            return "Retry the connection; if it persists, re-check the account and the topic configuration.";
        case DOCK_DIAG_ERR_TOPIC_MISMATCH:
            return "The topic must be thing/product/<client_id>/<channel>.";
        case DOCK_DIAG_ERR_CLIENT_ID_MISMATCH:
            return "The owner segment must equal the login account <app_id>_esdk_<device_sn> exactly "
                   "(use dock_session_account(), not the bare SN).";
        case DOCK_DIAG_ERR_SUB_NOT_ALLOWED:
            return "Subscribe only to state_reply / events_reply / requests_reply / services / error.";
        case DOCK_DIAG_ERR_PUB_NOT_ALLOWED:
            return "Publish only to state / events / requests / osd / services_reply.";
        case DOCK_DIAG_ERR_QOS_MISMATCH:
            return "Use the QoS the channel requires (QoS 1 for state / events).";
        case DOCK_DIAG_ERR_RATE_LIMIT:
            return "Uplink is capped at 5 Hz; keep at least 200 ms between messages.";
        case DOCK_DIAG_ERR_PAYLOAD_TOO_LARGE:
            return "A single message is capped at 2048 bytes; shrink the payload, or declare a device "
                   "type whose access raises the limit.";
        default:
            return "Not a documented code -- capture the wire log and report it.";
    }
}

/* ---- CONNACK ------------------------------------------------------------------------------ */

const char *dock_diag_connack_str(int connack_rc)
{
    switch (connack_rc) {
        case 0: return "connection accepted";
        case 1: return "unacceptable protocol version";
        case 2: return "identifier rejected";
        case 3: return "server unavailable";
        case 4: return "bad user name or password";
        case 5: return "not authorised";
        default: return "reserved / non-standard CONNACK code";
    }
}

const char *dock_diag_connack_hint(int connack_rc)
{
    switch (connack_rc) {
        case 0: return "Normal flow: subscribe to the reply topics and start reporting.";
        case 1: return "Check the MQTT protocol version the client negotiates against the broker.";
        case 2: return "Check the client id is non-empty and follows the account rule.";
        case 3: return "The ground station service is temporarily unavailable; retry later.";
        case 4: return "Check the username/password format: username <app_id>_esdk_<device_sn>, "
                       "password <app_key>_<app_license>.";
        case 5: return "app_id / app_key / app_license failed validation -- or another client is "
                       "already connected with this same account (the second one is refused).";
        default: return "Not defined by MQTT 3.1.1; capture the CONNACK bytes.";
    }
}

/* ---- Rendering ---------------------------------------------------------------------------- */

int dock_diag_format(const DockDiagEvent_t *ev, char *buf, size_t cap)
{
    if (!buf || cap == 0) return 0;
    if (!ev) { buf[0] = '\0'; return 0; }
    /* One snprintf, one line: callers may be on the MQTT receive thread, where a second print
     * would interleave with whatever the main thread is writing. */
    return snprintf(buf, cap, "[diag] %lld %s | refused=%s%s%s",
                    (long long)ev->error_code,
                    dock_diag_error_str(ev->error_code),
                    ev->topic[0] ? ev->topic : "(not reported)",
                    ev->error_reason[0] ? " | " : "",
                    ev->error_reason[0] ? ev->error_reason : "");
}

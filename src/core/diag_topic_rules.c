/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Diagnostics part 2: the local topic-rule engine. Pure functions -- no session, no allocation.
 *
 * Predicts what the gateway's ACL would say about a topic, so a caller can compare the prediction
 * against what the gateway actually answers on /error. Where the two disagree, the documented rules
 * and the deployed ones have drifted -- which is a finding, not a bug in this file.
 */
#include "edge_dock/diag.h"

#include <string.h>

#define DIAG_TOPIC_PREFIX     "thing/product/"
#define DIAG_TOPIC_PREFIX_LEN (sizeof(DIAG_TOPIC_PREFIX) - 1)
#define DIAG_REQUIRED_QOS     1

/* The documented whitelists. Direction is from the third-party device's point of view: it PUBLISHES
 * its own state/telemetry and its requests to the dock, and SUBSCRIBES to the answers. */
static const char *const k_publishable[] = {
    "state", "events", "requests", "osd", "services_reply", NULL
};
static const char *const k_subscribable[] = {
    "state_reply", "events_reply", "requests_reply", "services", "error", NULL
};

static bool diag_in_list(const char *const *list, const char *s, size_t len)
{
    for (int i = 0; list[i]; i++) {
        if (strlen(list[i]) == len && strncmp(list[i], s, len) == 0) return true;
    }
    return false;
}

/**
 * Shared body. `allowed` is the whitelist for the direction being checked.
 *
 * Check ORDER is taken from observed gateway behaviour, not invented: real hardware answered
 * 16843010 (owner mismatch) for `thing/product/gnd/requests/get_file_list` -- a topic that is also
 * malformed, carrying an extra segment. So the gateway matches the prefix, then judges the owner,
 * and only afterwards looks at the channel. Checking the segment count first would predict 16843009
 * for that topic and disagree with the hardware.
 */
static DockDiagVerdict_t diag_check(const char *account, const char *topic, int qos,
                                    const char *const *allowed, DockDiagVerdict_t denied)
{
    if (!account || !account[0] || !topic || !topic[0]) return DOCK_DIAG_TOPIC_BAD_TEMPLATE;

    /* Wildcards never belong in a concrete topic: illegal in a PUBLISH per MQTT 3.1.1 3.3.2.1, and
     * meaningless against an exact-match whitelist on the subscribe side. */
    if (strchr(topic, '+') || strchr(topic, '#')) return DOCK_DIAG_TOPIC_BAD_TEMPLATE;

    if (strncmp(topic, DIAG_TOPIC_PREFIX, DIAG_TOPIC_PREFIX_LEN) != 0)
        return DOCK_DIAG_TOPIC_BAD_TEMPLATE;

    const char *owner = topic + DIAG_TOPIC_PREFIX_LEN;
    const char *slash = strchr(owner, '/');
    if (!slash || slash == owner) return DOCK_DIAG_TOPIC_BAD_TEMPLATE;

    size_t owner_len = (size_t)(slash - owner);
    if (strlen(account) != owner_len || strncmp(account, owner, owner_len) != 0)
        return DOCK_DIAG_TOPIC_OWNER_MISMATCH;

    const char *channel = slash + 1;
    if (!channel[0] || strchr(channel, '/')) return DOCK_DIAG_TOPIC_BAD_TEMPLATE;

    if (!diag_in_list(allowed, channel, strlen(channel))) return denied;

    /* qos<0 means "not checking QoS" -- used when the caller only wants the topic verdict. */
    if (qos >= 0 && qos != DIAG_REQUIRED_QOS) return DOCK_DIAG_TOPIC_QOS_DENIED;

    return DOCK_DIAG_TOPIC_OK;
}

DockDiagVerdict_t dock_diag_check_publish(const char *account, const char *topic, int qos)
{
    return diag_check(account, topic, qos, k_publishable, DOCK_DIAG_TOPIC_PUB_DENIED);
}

DockDiagVerdict_t dock_diag_check_subscribe(const char *account, const char *topic, int qos)
{
    return diag_check(account, topic, qos, k_subscribable, DOCK_DIAG_TOPIC_SUB_DENIED);
}

int64_t dock_diag_verdict_to_error_code(DockDiagVerdict_t v)
{
    switch (v) {
        case DOCK_DIAG_TOPIC_OK:              return 0;
        case DOCK_DIAG_TOPIC_BAD_TEMPLATE:    return DOCK_DIAG_ERR_TOPIC_MISMATCH;
        case DOCK_DIAG_TOPIC_OWNER_MISMATCH:  return DOCK_DIAG_ERR_CLIENT_ID_MISMATCH;
        case DOCK_DIAG_TOPIC_SUB_DENIED:      return DOCK_DIAG_ERR_SUB_NOT_ALLOWED;
        case DOCK_DIAG_TOPIC_PUB_DENIED:      return DOCK_DIAG_ERR_PUB_NOT_ALLOWED;
        case DOCK_DIAG_TOPIC_QOS_DENIED:      return DOCK_DIAG_ERR_QOS_MISMATCH;
        default:                              return 0;
    }
}

const char *dock_diag_verdict_str(DockDiagVerdict_t v)
{
    switch (v) {
        case DOCK_DIAG_TOPIC_OK:             return "allowed";
        case DOCK_DIAG_TOPIC_BAD_TEMPLATE:   return "bad topic template";
        case DOCK_DIAG_TOPIC_OWNER_MISMATCH: return "owner segment != account";
        case DOCK_DIAG_TOPIC_SUB_DENIED:     return "channel not subscribable";
        case DOCK_DIAG_TOPIC_PUB_DENIED:     return "channel not publishable";
        case DOCK_DIAG_TOPIC_QOS_DENIED:     return "wrong QoS for this channel";
        default:                             return "unknown verdict";
    }
}

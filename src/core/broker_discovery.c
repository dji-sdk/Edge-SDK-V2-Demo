/**
 ********************************************************************
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * Broker discovery (UDP broadcast) -- pure C implementation.
 * Ported from reference/sample_esdk2/src/mqtt/broker_discovery.cpp.
 ********************************************************************
 */
#include "edge_dock/broker_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define DISC_MAGIC      "MQTTBRK|"
#define DISC_MAGIC_LEN  8

/* In "MQTTBRK|v=1|id=..|name=..|ip=..|seq=..", look up the value of key= and copy it into out.
 * Returns 1 if found, otherwise 0. The value ends at '|' or the end of the string. */
static int find_kv(const char *msg, const char *key, char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "|%s=", key);   /* leading '|' avoids name= matching things like ...name= */
    const char *p = strstr(msg, pat);
    if (!p) {
        /* Handle the first field immediately following the magic: key comes right after "MQTTBRK|" */
        char pat0[32];
        snprintf(pat0, sizeof(pat0), "%s=", key);
        if (strncmp(msg + DISC_MAGIC_LEN, pat0, strlen(pat0)) == 0) {
            p = msg + DISC_MAGIC_LEN - 1;  /* let the +strlen(pat) below land at the value start */
        } else {
            return 0;
        }
    }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '|' && i + 1 < cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* Parse a broadcast frame. When valid (v==1 and ip non-empty), write ip into out_ip and return 1. */
static int parse_discover_message(const char *msg, char *out_ip, size_t cap)
{
    if (strncmp(msg, DISC_MAGIC, DISC_MAGIC_LEN) != 0) {
        return 0;   /* magic mismatch */
    }
    char ver[8] = {0};
    if (find_kv(msg, "v", ver, sizeof(ver)) && atoi(ver) != 1) {
        return 0;   /* version not supported */
    }
    char ip[46] = {0};
    if (!find_kv(msg, "ip", ip, sizeof(ip)) || ip[0] == '\0') {
        return 0;   /* no ip */
    }
    snprintf(out_ip, cap, "%s", ip);
    return 1;
}

int dock_discover_broker(char *out_ip, size_t out_ip_cap, int timeout_ms)
{
    if (!out_ip || out_ip_cap == 0) {
        return 0;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[discover] socket failed: %s\n", strerror(errno));
        return 0;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DOCK_DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;   /* bind 0.0.0.0 to receive broadcasts */
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[discover] bind 0.0.0.0:%d failed: %s\n",
                DOCK_DISCOVERY_PORT, strerror(errno));
        close(sock);
        return 0;
    }

    /* Each recv blocks at most 1s, used as a heartbeat tick + accumulated total timeout. */
    struct timeval tv = { 1, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    fprintf(stderr, "[discover] listening on UDP %d (timeout=%dms) ...\n",
            DOCK_DISCOVERY_PORT, timeout_ms);

    char buf[4096];
    int elapsed_ms = 0;
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr *)&src, &slen);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                elapsed_ms += 1000;
                if (timeout_ms > 0 && elapsed_ms >= timeout_ms) {
                    fprintf(stderr, "[discover] timeout, give up\n");
                    close(sock);
                    return 0;
                }
                continue;   /* heartbeat, keep waiting */
            }
            fprintf(stderr, "[discover] recvfrom error: %s\n", strerror(errno));
            continue;
        }
        if (len == 0) {
            continue;
        }
        buf[len] = '\0';

        if (parse_discover_message(buf, out_ip, out_ip_cap)) {
            fprintf(stderr, "[discover] -> broker ip=%s\n", out_ip);
            close(sock);
            return 1;
        }
        /* invalid frame, keep waiting */
    }
}

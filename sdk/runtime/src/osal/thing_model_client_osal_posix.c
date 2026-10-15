/**
 * @file    thing_model_client_osal_posix.c
 * @brief   OSAL implementation for Linux/macOS using pthread + BSD sockets.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client_log.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---- mutex ---- */
static void *posix_mutex_create(void) {
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(m, NULL);
    return m;
}
static void posix_mutex_lock(void *m)    { pthread_mutex_lock(m); }
static void posix_mutex_unlock(void *m)  { pthread_mutex_unlock(m); }
static void posix_mutex_destroy(void *m) { if (m) { pthread_mutex_destroy(m); free(m); } }

/* ---- event (binary semaphore via POSIX sem_t) ---- */
static void *posix_event_create(void) {
    sem_t *s = malloc(sizeof(*s));
    if (!s) return NULL;
    if (sem_init(s, 0, 0) != 0) { free(s); return NULL; }
    return s;
}
static int posix_event_wait(void *ev, uint32_t timeout_ms) {
    sem_t *s = ev;
#ifdef __APPLE__
    while (timeout_ms > 0) {
        if (sem_trywait(s) == 0) return THING_MODEL_RC_OK;
        if (errno != EAGAIN) return -1;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (timeout_ms >= 100) timeout_ms -= 100; else timeout_ms = 0;
    }
    if (sem_trywait(s) == 0) return THING_MODEL_RC_OK;
    return THING_MODEL_RC_TIMEOUT;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    if (sem_timedwait(s, &ts) == 0) return THING_MODEL_RC_OK;
    if (errno == ETIMEDOUT) return THING_MODEL_RC_TIMEOUT;
    return -1;
#endif
}
static void posix_event_signal(void *ev) { sem_post(ev); }
static void posix_event_destroy(void *ev) { if (ev) { sem_destroy(ev); free(ev); } }

/* ---- thread ---- */
typedef struct { pthread_t tid; void (*entry)(void *); void *arg; } posix_thread_t;
static void *posix_thread_trampoline(void *p) {
    posix_thread_t *t = p;
    t->entry(t->arg);
    return NULL;
}
static void *posix_thread_create(void (*entry)(void *), void *arg,
                                 const char *name, size_t stack_sz) {
    (void)name; (void)stack_sz;
    posix_thread_t *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->entry = entry; t->arg = arg;
    if (pthread_create(&t->tid, NULL, posix_thread_trampoline, t) != 0) {
        free(t); return NULL;
    }
#if defined(__linux__)
    if (name) pthread_setname_np(t->tid, name);
#endif
    return t;
}
static void posix_thread_destroy(void *th) {
    if (!th) return;
    posix_thread_t *t = th;
    pthread_join(t->tid, NULL);
    free(t);
}
static void posix_thread_yield(void) { sched_yield(); }

/* ---- time ---- */
static uint64_t posix_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
static uint64_t posix_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
static void posix_sleep_ms(uint32_t ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- socket: direct libc passthrough ---- */
static int posix_socket(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}
static int posix_close(int fd) {
    return close(fd);
}
static int posix_bind(int fd, const T_TmSockAddr *addr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = addr->family;
    sa.sin_port   = htons(addr->port);
    sa.sin_addr.s_addr = addr->addr_v4;
    return bind(fd, (struct sockaddr *)&sa, sizeof(sa));
}
static int posix_listen(int fd, int backlog) {
    return listen(fd, backlog);
}
static int posix_accept(int fd, T_TmSockAddr *peer_out) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int cfd = accept(fd, (struct sockaddr *)&sa, &sl);
    if (cfd < 0) return -1;
    if (peer_out) {
        peer_out->family = sa.sin_family;
        peer_out->port   = ntohs(sa.sin_port);
        peer_out->addr_v4 = sa.sin_addr.s_addr;
    }
    return cfd;
}
static int posix_connect(int fd, const T_TmSockAddr *addr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = addr->family;
    sa.sin_port   = htons(addr->port);
    sa.sin_addr.s_addr = addr->addr_v4;
    return connect(fd, (struct sockaddr *)&sa, sizeof(sa));
}
static ssize_t posix_send(int fd, const void *buf, size_t len, int flags) {
    return send(fd, buf, len, flags);
}
static ssize_t posix_recv(int fd, void *buf, size_t len, int flags) {
    return recv(fd, buf, len, flags);
}
static int posix_setsockopt(int fd, int level, int optname, const void *val, size_t len) {
    return setsockopt(fd, level, optname, val, (socklen_t)len);
}
static int posix_getsockopt(int fd, int level, int optname, void *val, size_t *len) {
    socklen_t sl = (socklen_t)*len;
    int r = getsockopt(fd, level, optname, val, &sl);
    *len = sl;
    return r;
}
static int posix_set_nonblock(int fd, bool nonblock) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
static int posix_shutdown(int fd, int how) {
    return shutdown(fd, how);
}
static int posix_poll(T_TmPollFd *fds, size_t n, int timeout_ms) {
    return poll((struct pollfd *)fds, (nfds_t)n, timeout_ms);
}
static int posix_af_to_native(int af) {
    switch (af) {
        case TM_AF_INET:  return AF_INET;
        case TM_AF_INET6: return AF_INET6;
        default:          return -1;
    }
}
static int posix_inet_pton(int af, const char *src, void *dst) {
    int naf = posix_af_to_native(af);
    if (naf < 0) return -1;
    return inet_pton(naf, src, dst) == 1 ? 0 : -1;   /* POSIX returns 1 on success; normalize to 0 */
}
static int posix_inet_ntop(int af, const void *src, char *dst, size_t size) {
    int naf = posix_af_to_native(af);
    if (naf < 0) return -1;
    return inet_ntop(naf, src, dst, (socklen_t)size) ? 0 : -1;
}

static const T_ThingModelOsal POSIX_OSAL = {
    .mutex_create  = posix_mutex_create,
    .mutex_lock    = posix_mutex_lock,
    .mutex_unlock  = posix_mutex_unlock,
    .mutex_destroy = posix_mutex_destroy,

    .event_create  = posix_event_create,
    .event_wait    = posix_event_wait,
    .event_signal  = posix_event_signal,
    .event_destroy = posix_event_destroy,

    .thread_create  = posix_thread_create,
    .thread_destroy = posix_thread_destroy,
    .thread_yield   = posix_thread_yield,

    .time_ms  = posix_time_ms,
    .sleep_ms = posix_sleep_ms,

    .mem_alloc = malloc,
    .mem_free  = free,

    .socket        = posix_socket,
    .close         = posix_close,
    .bind          = posix_bind,
    .listen        = posix_listen,
    .accept        = posix_accept,
    .connect       = posix_connect,
    .send          = posix_send,
    .recv          = posix_recv,
    .setsockopt    = posix_setsockopt,
    .getsockopt    = posix_getsockopt,
    .set_nonblock  = posix_set_nonblock,
    .shutdown      = posix_shutdown,
    .poll          = posix_poll,
    .inet_pton     = posix_inet_pton,
    .inet_ntop     = posix_inet_ntop,

    .time_us = posix_time_us,
};

void ThingModelOsal_RegisterPosix(void)
{
    ThingModelOsal_RegisterEx(&POSIX_OSAL, sizeof(POSIX_OSAL));
}

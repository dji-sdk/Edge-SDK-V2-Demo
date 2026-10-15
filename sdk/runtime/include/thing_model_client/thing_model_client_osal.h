/**
 * @file    thing_model_client_osal.h
 * @brief   Platform Abstraction Layer.
 *          The runtime does not directly #include pthread.h / FreeRTOS.h, everything goes through this vtable.
 *          The implementation is selected via the macros THING_MODEL_OSAL_POSIX / THING_MODEL_OSAL_FREERTOS / THING_MODEL_OSAL_CUSTOM.
 */
#ifndef THING_MODEL_CLIENT_OSAL_H
#define THING_MODEL_CLIENT_OSAL_H

#include "thing_model_client_types.h"
#include <sys/types.h>   /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Socket constants — OSAL own namespace, does not leak <sys/socket.h>.
 * Values are chosen to match POSIX so the POSIX backend can pass them through
 * directly; lwIP backend maps them at the function boundary.
 * ============================================================ */
#define TM_AF_INET         2
#define TM_AF_INET6        10
#define TM_SOCK_STREAM      1
#define TM_SOCK_DGRAM       2

#define TM_SOL_SOCKET       0xFFFF
#define TM_IPPROTO_TCP      6
#define TM_SO_REUSEADDR     4
#define TM_SO_KEEPALIVE     9
#define TM_SO_SNDTIMEO      21
#define TM_SO_RCVTIMEO      22
#define TM_SO_ERROR         23
#define TM_TCP_NODELAY      1

#define TM_MSG_NOSIGNAL     0x4000

#define TM_SHUT_RD          0
#define TM_SHUT_WR          1
#define TM_SHUT_RDWR        2

#define TM_POLLIN           0x001
#define TM_POLLOUT          0x004
#define TM_POLLERR          0x008
#define TM_POLLHUP          0x010

typedef struct {
    uint16_t family;
    uint16_t port;
    uint32_t addr_v4;
} T_TmSockAddr;

typedef struct {
    int   fd;
    short events;
    short revents;
} T_TmPollFd;

typedef struct {
    /* ---- mutex ---- */
    void *(*mutex_create)(void);
    void  (*mutex_lock)(void *m);
    void  (*mutex_unlock)(void *m);
    void  (*mutex_destroy)(void *m);

    /* ---- binary event (semaphore equivalent), used for synchronous wait ---- */
    void *(*event_create)(void);
    /** @return 0=signaled, THING_MODEL_RC_TIMEOUT=timeout, <0=error */
    int   (*event_wait)(void *ev, uint32_t timeout_ms);
    void  (*event_signal)(void *ev);
    void  (*event_destroy)(void *ev);

    /* ---- thread (used by dispatcher worker) ---- */
    void *(*thread_create)(void (*entry)(void *arg), void *arg, const char *name, size_t stack_sz);
    void  (*thread_destroy)(void *t);
    void  (*thread_yield)(void);

    /* ---- time ---- */
    uint64_t (*time_ms)(void);
    void     (*sleep_ms)(uint32_t ms);

    /* ---- memory (optional, falls back to malloc/free if NULL) ---- */
    void *(*mem_alloc)(size_t sz);
    void  (*mem_free)(void *p);

     /* ============================================================
     * v0.3 extension: socket (POSIX-shaped) + time_us.
     * Register() copies only the v0.2 prefix above; RegisterEx() copies
     * the full struct including these fields.
     * ============================================================ */

    /* ---- socket: one-to-one POSIX mapping, int fd ---- */
    int      (*socket)       (int domain, int type, int protocol);
    int      (*close)        (int fd);
    int      (*bind)         (int fd, const T_TmSockAddr *addr);
    int      (*listen)       (int fd, int backlog);
    int      (*accept)       (int fd, T_TmSockAddr *peer_out);
    int      (*connect)      (int fd, const T_TmSockAddr *addr);
    ssize_t  (*send)         (int fd, const void *buf, size_t len, int flags);
    ssize_t  (*recv)         (int fd, void *buf, size_t len, int flags);
    int      (*setsockopt)   (int fd, int level, int optname, const void *val, size_t len);
    int      (*getsockopt)   (int fd, int level, int optname, void *val, size_t *len);
    int      (*set_nonblock) (int fd, bool nonblock);
    int      (*shutdown)     (int fd, int how);
    int      (*poll)         (T_TmPollFd *fds, size_t n, int timeout_ms);
    int      (*inet_pton)    (int af, const char *src, void *dst);
    int      (*inet_ntop)    (int af, const void *src, char *dst, size_t size);

    /* ---- microsecond monotonic timestamp ---- */
    uint64_t (*time_us)(void);
} T_ThingModelOsal;

/** Register a v0.2-compatible OSAL (copies only the original prefix). */
void              ThingModelOsal_Register(const T_ThingModelOsal *osal);
/** Register an OSAL with explicit struct size, including v0.3 extensions. */
void              ThingModelOsal_RegisterEx(const T_ThingModelOsal *osal, size_t osal_size);
const T_ThingModelOsal * ThingModelOsal_Get(void);

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
void ThingModelOsal_RegisterPosix(void);
#endif

void ThingModelOsal_RegisterFreeRtos(void);

/* ============================================================
 * Global memory helpers
 * ----------------------------------------------------------------
 * Go through the mem_alloc/mem_free of the currently registered OSAL vtable; when the vtable is not
 * registered or mem_alloc==NULL, fall back to libc malloc/free.
 *
 * Both the SDK's own code and runtime-internal allocations go through this set, so a custom memory
 * pool (static pool / tracking allocator etc.) can cover the entire
 * SDK's heap allocation without being bypassed.
 *
 * Difference from calling `mem_alloc/free` directly: this layer does NULL/0 fallback + free(NULL) no-op,
 * aligned with libc semantics, so the caller need not check each time.
 * ============================================================ */
void  *ThingModelMem_Alloc(size_t sz);
void  *ThingModelMem_Calloc(size_t nmemb, size_t sz);
void   ThingModelMem_Free(void *p);
char  *ThingModelMem_Strdup(const char *s);

/* ============================================================
 * OSAL facade helpers
 * ============================================================ */

uint64_t ThingModelOsal_TimeMs (void);
uint64_t ThingModelOsal_TimeUs (void);
void     ThingModelOsal_SleepMs(uint32_t ms);
void     ThingModelOsal_ThreadYield(void);

typedef struct { void *_h; } T_ThingModelMutex;
T_ThingModelMutex ThingModelOsal_MutexCreate (void);
void ThingModelOsal_MutexLock   (T_ThingModelMutex m);
void ThingModelOsal_MutexUnlock (T_ThingModelMutex m);
void ThingModelOsal_MutexDestroy(T_ThingModelMutex m);

typedef struct { void *_h; } T_ThingModelEvent;
T_ThingModelEvent ThingModelOsal_EventCreate (void);
int  ThingModelOsal_EventWait   (T_ThingModelEvent e, uint32_t timeout_ms);
void ThingModelOsal_EventSignal (T_ThingModelEvent e);
void ThingModelOsal_EventDestroy(T_ThingModelEvent e);

typedef struct { void *_h; } T_ThingModelThread;
T_ThingModelThread ThingModelOsal_ThreadCreate(void (*entry)(void *), void *arg,
                                               const char *name, size_t stack_bytes);
void ThingModelOsal_ThreadJoin(T_ThingModelThread t);

/* ---- socket (POSIX-shaped, int fd) ---- */
int     ThingModelOsal_Socket     (int domain, int type, int protocol);
int     ThingModelOsal_Close      (int fd);
int     ThingModelOsal_Bind       (int fd, const T_TmSockAddr *addr);
int     ThingModelOsal_Listen     (int fd, int backlog);
int     ThingModelOsal_Accept     (int fd, T_TmSockAddr *peer_out);
int     ThingModelOsal_Connect    (int fd, const T_TmSockAddr *addr);
ssize_t ThingModelOsal_Send       (int fd, const void *buf, size_t len, int flags);
ssize_t ThingModelOsal_Recv       (int fd, void *buf, size_t len, int flags);
int     ThingModelOsal_Setsockopt (int fd, int level, int optname, const void *val, size_t len);
int     ThingModelOsal_Getsockopt (int fd, int level, int optname, void *val, size_t *len);
int     ThingModelOsal_SetNonblock(int fd, bool nonblock);
int     ThingModelOsal_Shutdown   (int fd, int how);
int     ThingModelOsal_Poll       (T_TmPollFd *fds, size_t n, int timeout_ms);
int     ThingModelOsal_InetPton (int af, const char *src, void *dst);
int     ThingModelOsal_InetNtop (int af, const void *src, char *dst, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_OSAL_H */

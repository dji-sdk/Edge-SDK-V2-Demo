/**
 * @file    thing_model_client_osal.c
 * @brief   OSAL register/get + memory helpers + facade helpers.
 */
#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client_log.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static T_ThingModelOsal s_osal_storage;
static const T_ThingModelOsal *s_osal = NULL;

void ThingModelOsal_RegisterEx(const T_ThingModelOsal *osal, size_t osal_size)
{
    if (!osal) { s_osal = NULL; return; }
    memset(&s_osal_storage, 0, sizeof(s_osal_storage));
    if (osal_size > sizeof(s_osal_storage)) osal_size = sizeof(s_osal_storage);
    memcpy(&s_osal_storage, osal, osal_size);
    s_osal = &s_osal_storage;
}

void ThingModelOsal_Register(const T_ThingModelOsal *osal)
{
    ThingModelOsal_RegisterEx(osal, offsetof(T_ThingModelOsal, socket));
}

const T_ThingModelOsal *ThingModelOsal_Get(void) { return s_osal; }

void *ThingModelMem_Alloc(size_t sz)
{
    if (sz == 0) return NULL;
    if (s_osal && s_osal->mem_alloc) return s_osal->mem_alloc(sz);
    return malloc(sz);
}

void *ThingModelMem_Calloc(size_t nmemb, size_t sz)
{
    if (nmemb == 0 || sz == 0) return NULL;
    size_t total = nmemb * sz;
    if (sz != 0 && total / sz != nmemb) return NULL;
    void *p = ThingModelMem_Alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void ThingModelMem_Free(void *p)
{
    if (!p) return;
    if (s_osal && s_osal->mem_free) { s_osal->mem_free(p); return; }
    free(p);
}

char *ThingModelMem_Strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)ThingModelMem_Alloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static void warn_once(const char *feature, int *seen)
{
    if (*seen) return;
    *seen = 1;
    LOG_ERROR("[osal] %s not supported by the registered backend", feature);
}

uint64_t ThingModelOsal_TimeMs(void)
{
    if (s_osal && s_osal->time_ms) return s_osal->time_ms();
    return 0;
}

uint64_t ThingModelOsal_TimeUs(void)
{
    if (s_osal && s_osal->time_us) return s_osal->time_us();
    if (s_osal && s_osal->time_ms) return s_osal->time_ms() * 1000ULL;
    return 0;
}

void ThingModelOsal_SleepMs(uint32_t ms)
{
    if (s_osal && s_osal->sleep_ms) s_osal->sleep_ms(ms);
}

void ThingModelOsal_ThreadYield(void)
{
    if (s_osal && s_osal->thread_yield) s_osal->thread_yield();
}

T_ThingModelMutex ThingModelOsal_MutexCreate(void)
{
    T_ThingModelMutex m = { NULL };
    if (s_osal && s_osal->mutex_create) m._h = s_osal->mutex_create();
    return m;
}
void ThingModelOsal_MutexLock   (T_ThingModelMutex m) { if (m._h && s_osal && s_osal->mutex_lock)   s_osal->mutex_lock(m._h); }
void ThingModelOsal_MutexUnlock (T_ThingModelMutex m) { if (m._h && s_osal && s_osal->mutex_unlock) s_osal->mutex_unlock(m._h); }
void ThingModelOsal_MutexDestroy(T_ThingModelMutex m) { if (m._h && s_osal && s_osal->mutex_destroy) s_osal->mutex_destroy(m._h); }

T_ThingModelEvent ThingModelOsal_EventCreate(void)
{
    T_ThingModelEvent e = { NULL };
    if (s_osal && s_osal->event_create) e._h = s_osal->event_create();
    return e;
}
int ThingModelOsal_EventWait(T_ThingModelEvent e, uint32_t timeout_ms)
{
    if (!e._h || !s_osal || !s_osal->event_wait) return THING_MODEL_RC_INVALID_ARG;
    return s_osal->event_wait(e._h, timeout_ms);
}
void ThingModelOsal_EventSignal (T_ThingModelEvent e) { if (e._h && s_osal && s_osal->event_signal)  s_osal->event_signal(e._h); }
void ThingModelOsal_EventDestroy(T_ThingModelEvent e) { if (e._h && s_osal && s_osal->event_destroy) s_osal->event_destroy(e._h); }

T_ThingModelThread ThingModelOsal_ThreadCreate(void (*entry)(void *), void *arg,
                                               const char *name, size_t stack_bytes)
{
    T_ThingModelThread t = { NULL };
    if (s_osal && s_osal->thread_create)
        t._h = s_osal->thread_create(entry, arg, name, stack_bytes);
    return t;
}
void ThingModelOsal_ThreadJoin(T_ThingModelThread t)
{
    if (t._h && s_osal && s_osal->thread_destroy) s_osal->thread_destroy(t._h);
}

static int s_warn_sock = 0;

int     ThingModelOsal_Socket     (int d, int t, int p) { if (!s_osal||!s_osal->socket)       { warn_once("socket",&s_warn_sock); return -1; } return s_osal->socket(d,t,p); }
int     ThingModelOsal_Close      (int fd)             { if (!s_osal||!s_osal->close)         { return -1; } return s_osal->close(fd); }
int     ThingModelOsal_Bind       (int fd, const T_TmSockAddr *a) { if (!s_osal||!s_osal->bind) return -1; return s_osal->bind(fd,a); }
int     ThingModelOsal_Listen     (int fd, int bl)     { if (!s_osal||!s_osal->listen)       return -1; return s_osal->listen(fd,bl); }
int     ThingModelOsal_Accept     (int fd, T_TmSockAddr *p) { if (!s_osal||!s_osal->accept) return -1; return s_osal->accept(fd,p); }
int     ThingModelOsal_Connect    (int fd, const T_TmSockAddr *a) { if (!s_osal||!s_osal->connect) return -1; return s_osal->connect(fd,a); }
ssize_t ThingModelOsal_Send       (int fd, const void *b, size_t n, int f) { if (!s_osal||!s_osal->send) return -1; return s_osal->send(fd,b,n,f); }
ssize_t ThingModelOsal_Recv       (int fd, void *b, size_t n, int f) { if (!s_osal||!s_osal->recv) return -1; return s_osal->recv(fd,b,n,f); }
int     ThingModelOsal_Setsockopt (int fd, int l, int o, const void *v, size_t n) { if (!s_osal||!s_osal->setsockopt) return -1; return s_osal->setsockopt(fd,l,o,v,n); }
int     ThingModelOsal_Getsockopt (int fd, int l, int o, void *v, size_t *n) { if (!s_osal||!s_osal->getsockopt) return -1; return s_osal->getsockopt(fd,l,o,v,n); }
int     ThingModelOsal_SetNonblock(int fd, bool nb)   { if (!s_osal||!s_osal->set_nonblock) return -1; return s_osal->set_nonblock(fd,nb); }
int     ThingModelOsal_Shutdown   (int fd, int how)   { if (!s_osal||!s_osal->shutdown)     return -1; return s_osal->shutdown(fd,how); }
int     ThingModelOsal_Poll       (T_TmPollFd *fds, size_t n, int ms) { if (!s_osal||!s_osal->poll) return -1; return s_osal->poll(fds,n,ms); }
int     ThingModelOsal_InetPton (int af, const char *src, void *dst) { if (!s_osal||!s_osal->inet_pton) return -1; return s_osal->inet_pton(af,src,dst); }
int     ThingModelOsal_InetNtop (int af, const void *src, char *dst, size_t size) { if (!s_osal||!s_osal->inet_ntop) return -1; return s_osal->inet_ntop(af,src,dst,size); }

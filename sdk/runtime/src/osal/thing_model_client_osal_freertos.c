/**
 * @file    thing_model_client_osal_freertos.c
 * @brief   FreeRTOS implementation of the T_ThingModelOsal vtable.
 */
#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client_log.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdlib.h>
#include <string.h>

/* ---- mutex ---- */
static void *fr_mutex_create(void) { return (void *)xSemaphoreCreateMutex(); }
static void fr_mutex_lock(void *m) { if (m) xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY); }
static void fr_mutex_unlock(void *m) { if (m) xSemaphoreGive((SemaphoreHandle_t)m); }
static void fr_mutex_destroy(void *m) { if (m) vSemaphoreDelete((SemaphoreHandle_t)m); }

/* ---- event (binary semaphore) ---- */
static void *fr_event_create(void) { return (void *)xSemaphoreCreateBinary(); }
static int fr_event_wait(void *ev, uint32_t timeout_ms) {
    if (!ev) return THING_MODEL_RC_INVALID_ARG;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTake((SemaphoreHandle_t)ev, ticks) == pdTRUE) ? THING_MODEL_RC_OK : THING_MODEL_RC_TIMEOUT;
}
static void fr_event_signal(void *ev) { if (ev) xSemaphoreGive((SemaphoreHandle_t)ev); }
static void fr_event_destroy(void *ev) { if (ev) vSemaphoreDelete((SemaphoreHandle_t)ev); }

/* ---- thread ---- */
typedef struct { TaskHandle_t task; void (*entry)(void *); void *arg; } fr_thread_t;
#ifndef THING_MODEL_OSAL_FREERTOS_DEFAULT_STACK
#define THING_MODEL_OSAL_FREERTOS_DEFAULT_STACK  4096
#endif
#ifndef THING_MODEL_OSAL_FREERTOS_DEFAULT_PRIO
#define THING_MODEL_OSAL_FREERTOS_DEFAULT_PRIO   (tskIDLE_PRIORITY + 2)
#endif
static void fr_thread_trampoline(void *p) {
    fr_thread_t *t = (fr_thread_t *)p;
    t->entry(t->arg);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void *fr_thread_create(void (*entry)(void *), void *arg, const char *name, size_t stack_sz) {
    if (!entry) return NULL;
    fr_thread_t *t = (fr_thread_t *)pvPortMalloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->entry = entry; t->arg = arg;
    size_t bytes = stack_sz ? stack_sz : THING_MODEL_OSAL_FREERTOS_DEFAULT_STACK;
    configSTACK_DEPTH_TYPE words = (configSTACK_DEPTH_TYPE)((bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t));
    if (xTaskCreate(fr_thread_trampoline, name ? name : "tm_thread", words, t,
                    THING_MODEL_OSAL_FREERTOS_DEFAULT_PRIO, &t->task) != pdPASS) {
        vPortFree(t); return NULL;
    }
    return t;
}
static void fr_thread_destroy(void *th) {
    if (!th) return;
    fr_thread_t *t = (fr_thread_t *)th;
    if (t->task) vTaskDelete(t->task);
    vPortFree(t);
}
static void fr_thread_yield(void) { taskYIELD(); }

/* ---- time ---- */
static uint64_t fr_time_ms(void) { return (uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS; }
static uint64_t fr_time_us(void) { return fr_time_ms() * 1000ULL; }
static void fr_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* ---- memory ---- */
static void *fr_mem_alloc(size_t sz) { return pvPortMalloc(sz); }
static void fr_mem_free(void *p) { if (p) vPortFree(p); }

static const T_ThingModelOsal FREERTOS_OSAL = {
    .mutex_create  = fr_mutex_create,
    .mutex_lock    = fr_mutex_lock,
    .mutex_unlock  = fr_mutex_unlock,
    .mutex_destroy = fr_mutex_destroy,

    .event_create  = fr_event_create,
    .event_wait    = fr_event_wait,
    .event_signal  = fr_event_signal,
    .event_destroy = fr_event_destroy,

    .thread_create  = fr_thread_create,
    .thread_destroy = fr_thread_destroy,
    .thread_yield   = fr_thread_yield,

    .time_ms  = fr_time_ms,
    .sleep_ms = fr_sleep_ms,

    .mem_alloc = fr_mem_alloc,
    .mem_free  = fr_mem_free,

    .socket        = NULL,
    .close         = NULL,
    .bind          = NULL,
    .listen        = NULL,
    .accept        = NULL,
    .connect       = NULL,
    .send          = NULL,
    .recv          = NULL,
    .setsockopt    = NULL,
    .getsockopt    = NULL,
    .set_nonblock  = NULL,
    .shutdown      = NULL,
    .poll          = NULL,
    .inet_pton     = NULL,
    .inet_ntop     = NULL,

    .time_us       = fr_time_us,
};

void ThingModelOsal_RegisterFreeRtos(void) {
    ThingModelOsal_RegisterEx(&FREERTOS_OSAL, sizeof(FREERTOS_OSAL));
}

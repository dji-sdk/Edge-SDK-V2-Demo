/**
 * @file    thing_model_client_codec.c
 * @brief   Thin cJSON wrapper implementation.
 */
#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#define J(p) ((cJSON *)(p))

/* ============================================================
 * Route cJSON's internal allocation through ThingModelMem_* (and thus through OSAL.mem_alloc/free).
 * On the first entry into any New/Parse, InitHooks once; not concurrency-safe (assumes a single thread
 * completes the first call, consistent with the SDK init path); subsequent multi-thread calls are safe.
 * ============================================================ */
static void *_cjson_malloc(size_t sz) { return ThingModelMem_Alloc(sz); }
static void  _cjson_free  (void *p)   { ThingModelMem_Free(p); }
static int s_cjson_hooks_inited = 0;
static void _ensure_cjson_hooks(void)
{
    if (s_cjson_hooks_inited) return;
    cJSON_Hooks h = { _cjson_malloc, _cjson_free };
    cJSON_InitHooks(&h);
    s_cjson_hooks_inited = 1;
}

ThingModelJsonObj_t *ThingModelJson_NewObject(void) { _ensure_cjson_hooks(); return (ThingModelJsonObj_t *)cJSON_CreateObject(); }
ThingModelJsonObj_t *ThingModelJson_NewArray(void)  { _ensure_cjson_hooks(); return (ThingModelJsonObj_t *)cJSON_CreateArray(); }
void          ThingModelJson_Free(ThingModelJsonObj_t *root) { cJSON_Delete(J(root)); }

void ThingModelJson_AddInt   (ThingModelJsonObj_t *o, const char *k, int64_t v)        { cJSON_AddNumberToObject(J(o), k, (double)v); }
void ThingModelJson_AddUInt  (ThingModelJsonObj_t *o, const char *k, uint64_t v)       { cJSON_AddNumberToObject(J(o), k, (double)v); }
void ThingModelJson_AddDouble(ThingModelJsonObj_t *o, const char *k, double v)         { cJSON_AddNumberToObject(J(o), k, v); }
void ThingModelJson_AddBool  (ThingModelJsonObj_t *o, const char *k, bool v)           { cJSON_AddBoolToObject(J(o), k, v); }
void ThingModelJson_AddString(ThingModelJsonObj_t *o, const char *k, const char *s)    { cJSON_AddStringToObject(J(o), k, s ? s : ""); }
void ThingModelJson_AddObject(ThingModelJsonObj_t *o, const char *k, ThingModelJsonObj_t *c)  { cJSON_AddItemToObject(J(o), k, J(c)); }
void ThingModelJson_AddArray (ThingModelJsonObj_t *o, const char *k, ThingModelJsonObj_t *a)  { cJSON_AddItemToObject(J(o), k, J(a)); }

void ThingModelJson_ArrAppendInt   (ThingModelJsonObj_t *a, int64_t v)       { cJSON_AddItemToArray(J(a), cJSON_CreateNumber((double)v)); }
void ThingModelJson_ArrAppendUInt  (ThingModelJsonObj_t *a, uint64_t v)      { cJSON_AddItemToArray(J(a), cJSON_CreateNumber((double)v)); }
void ThingModelJson_ArrAppendDouble(ThingModelJsonObj_t *a, double v)        { cJSON_AddItemToArray(J(a), cJSON_CreateNumber(v)); }
void ThingModelJson_ArrAppendBool  (ThingModelJsonObj_t *a, bool v)          { cJSON_AddItemToArray(J(a), cJSON_CreateBool(v)); }
void ThingModelJson_ArrAppendString(ThingModelJsonObj_t *a, const char *s)   { cJSON_AddItemToArray(J(a), cJSON_CreateString(s ? s : "")); }
void ThingModelJson_ArrAppendObject(ThingModelJsonObj_t *a, ThingModelJsonObj_t *c) { cJSON_AddItemToArray(J(a), J(c)); }

char *ThingModelJson_Print(const ThingModelJsonObj_t *r)        { return cJSON_Print(J(r)); }
char *ThingModelJson_PrintCompact(const ThingModelJsonObj_t *r) { return cJSON_PrintUnformatted(J(r)); }

ThingModelJsonObj_t *ThingModelJson_Parse(const char *t)               { _ensure_cjson_hooks(); return (ThingModelJsonObj_t *)cJSON_Parse(t); }
ThingModelJsonObj_t *ThingModelJson_ParseN(const char *t, size_t n)    { _ensure_cjson_hooks(); return (ThingModelJsonObj_t *)cJSON_ParseWithLength(t, n); }

ThingModelJsonObj_t *ThingModelJson_GetItem(const ThingModelJsonObj_t *o, const char *k) { return (ThingModelJsonObj_t *)cJSON_GetObjectItem(J(o), k); }
int           ThingModelJson_HasItem(const ThingModelJsonObj_t *o, const char *k) { return cJSON_HasObjectItem(J(o), k); }

int ThingModelJson_IsObject(const ThingModelJsonObj_t *o) { return cJSON_IsObject(J(o)); }
int ThingModelJson_IsArray (const ThingModelJsonObj_t *o) { return cJSON_IsArray(J(o)); }
int ThingModelJson_IsNumber(const ThingModelJsonObj_t *o) { return cJSON_IsNumber(J(o)); }
int ThingModelJson_IsString(const ThingModelJsonObj_t *o) { return cJSON_IsString(J(o)); }
int ThingModelJson_IsBool  (const ThingModelJsonObj_t *o) { return cJSON_IsBool(J(o)); }

int64_t      ThingModelJson_GetInt   (const ThingModelJsonObj_t *o, int64_t def)            { return cJSON_IsNumber(J(o)) ? (int64_t)J(o)->valuedouble : def; }
double       ThingModelJson_GetDouble(const ThingModelJsonObj_t *o, double def)             { return cJSON_IsNumber(J(o)) ? J(o)->valuedouble : def; }
bool         ThingModelJson_GetBool  (const ThingModelJsonObj_t *o, bool def)               { return cJSON_IsBool(J(o)) ? cJSON_IsTrue(J(o)) : (cJSON_IsNumber(J(o)) ? (J(o)->valuedouble != 0) : def); }
const char  *ThingModelJson_GetString(const ThingModelJsonObj_t *o, const char *def)        { return cJSON_IsString(J(o)) ? J(o)->valuestring : def; }
int          ThingModelJson_GetArraySize(const ThingModelJsonObj_t *a)                      { return cJSON_GetArraySize(J(a)); }
ThingModelJsonObj_t *ThingModelJson_GetArrayItem(const ThingModelJsonObj_t *a, int i)              { return (ThingModelJsonObj_t *)cJSON_GetArrayItem(J(a), i); }

int64_t      ThingModelJson_LookupInt   (const ThingModelJsonObj_t *o, const char *k, int64_t def)        { return ThingModelJson_GetInt(ThingModelJson_GetItem(o, k), def); }
double       ThingModelJson_LookupDouble(const ThingModelJsonObj_t *o, const char *k, double def)         { return ThingModelJson_GetDouble(ThingModelJson_GetItem(o, k), def); }
const char  *ThingModelJson_LookupString(const ThingModelJsonObj_t *o, const char *k, const char *def)    { return ThingModelJson_GetString(ThingModelJson_GetItem(o, k), def); }
bool         ThingModelJson_LookupBool  (const ThingModelJsonObj_t *o, const char *k, bool def)           { return ThingModelJson_GetBool(ThingModelJson_GetItem(o, k), def); }

/* ============================================================
 * hex encode/decode
 * ============================================================ */

char *ThingModelHex_Encode(const uint8_t *data, size_t len)
{
    char *out = (char *)ThingModelMem_Alloc(len * 2 + 1);
    if (!out) return NULL;
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = HEX[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEX[data[i] & 0xF];
    }
    out[len * 2] = 0;
    return out;
}

static int _hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

T_ThingModelReturnCode ThingModelHex_Decode(const char *hex, uint8_t **out_data, size_t *out_len)
{
    if (!out_data || !out_len) return THING_MODEL_RC_INVALID_ARG;
    *out_data = NULL;
    *out_len  = 0;
    if (!hex || !hex[0]) return THING_MODEL_RC_OK;
    size_t slen = strlen(hex);
    if (slen & 1) return THING_MODEL_RC_DECODE_ERROR;
    size_t n = slen / 2;
    uint8_t *buf = (uint8_t *)ThingModelMem_Alloc(n);
    if (!buf) return THING_MODEL_RC_OUT_OF_MEMORY;
    for (size_t i = 0; i < n; i++) {
        int hi = _hex_nibble(hex[i * 2]);
        int lo = _hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { ThingModelMem_Free(buf); return THING_MODEL_RC_DECODE_ERROR; }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_data = buf;
    *out_len  = n;
    return THING_MODEL_RC_OK;
}

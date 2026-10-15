/**
 * @file    thing_model_client_codec.h
 * @brief   Thin JSON codec wrapper (based on cJSON).
 *          This header is the only cJSON dependency exposed; callers using it need not include cJSON directly.
 */
#ifndef THING_MODEL_CLIENT_CODEC_H
#define THING_MODEL_CLIENT_CODEC_H

#include "thing_model_client_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThingModelJsonObj  ThingModelJsonObj_t;     /* opaque (= cJSON *) */

/* ---- Encode ---- */
ThingModelJsonObj_t *ThingModelJson_NewObject(void);
ThingModelJsonObj_t *ThingModelJson_NewArray(void);
void          ThingModelJson_Free(ThingModelJsonObj_t *root);

void ThingModelJson_AddInt   (ThingModelJsonObj_t *obj, const char *key, int64_t v);
void ThingModelJson_AddUInt  (ThingModelJsonObj_t *obj, const char *key, uint64_t v);
void ThingModelJson_AddDouble(ThingModelJsonObj_t *obj, const char *key, double v);
void ThingModelJson_AddBool  (ThingModelJsonObj_t *obj, const char *key, bool v);
void ThingModelJson_AddString(ThingModelJsonObj_t *obj, const char *key, const char *s);
void ThingModelJson_AddObject(ThingModelJsonObj_t *obj, const char *key, ThingModelJsonObj_t *child);
void ThingModelJson_AddArray (ThingModelJsonObj_t *obj, const char *key, ThingModelJsonObj_t *arr);

void ThingModelJson_ArrAppendInt   (ThingModelJsonObj_t *arr, int64_t v);
void ThingModelJson_ArrAppendUInt  (ThingModelJsonObj_t *arr, uint64_t v);
void ThingModelJson_ArrAppendDouble(ThingModelJsonObj_t *arr, double v);
void ThingModelJson_ArrAppendBool  (ThingModelJsonObj_t *arr, bool v);
void ThingModelJson_ArrAppendString(ThingModelJsonObj_t *arr, const char *s);
void ThingModelJson_ArrAppendObject(ThingModelJsonObj_t *arr, ThingModelJsonObj_t *child);

char *ThingModelJson_Print(const ThingModelJsonObj_t *root);
char *ThingModelJson_PrintCompact(const ThingModelJsonObj_t *root);

/* ---- Decode ---- */
ThingModelJsonObj_t *ThingModelJson_Parse(const char *text);
ThingModelJsonObj_t *ThingModelJson_ParseN(const char *text, size_t len);

ThingModelJsonObj_t *ThingModelJson_GetItem    (const ThingModelJsonObj_t *obj, const char *key);
int           ThingModelJson_HasItem    (const ThingModelJsonObj_t *obj, const char *key);
int           ThingModelJson_IsObject   (const ThingModelJsonObj_t *obj);
int           ThingModelJson_IsArray    (const ThingModelJsonObj_t *obj);
int           ThingModelJson_IsNumber   (const ThingModelJsonObj_t *obj);
int           ThingModelJson_IsString   (const ThingModelJsonObj_t *obj);
int           ThingModelJson_IsBool     (const ThingModelJsonObj_t *obj);

int64_t       ThingModelJson_GetInt     (const ThingModelJsonObj_t *obj, int64_t def);
double        ThingModelJson_GetDouble  (const ThingModelJsonObj_t *obj, double def);
bool          ThingModelJson_GetBool    (const ThingModelJsonObj_t *obj, bool def);
const char *  ThingModelJson_GetString  (const ThingModelJsonObj_t *obj, const char *def);
int           ThingModelJson_GetArraySize(const ThingModelJsonObj_t *arr);
ThingModelJsonObj_t *ThingModelJson_GetArrayItem(const ThingModelJsonObj_t *arr, int idx);

int64_t       ThingModelJson_LookupInt   (const ThingModelJsonObj_t *obj, const char *key, int64_t def);
double        ThingModelJson_LookupDouble(const ThingModelJsonObj_t *obj, const char *key, double def);
const char *  ThingModelJson_LookupString(const ThingModelJsonObj_t *obj, const char *key, const char *def);
bool          ThingModelJson_LookupBool  (const ThingModelJsonObj_t *obj, const char *key, bool def);

/* ---- hex encode/decode (for proto bytes fields; the wire side is an uppercase hex string) ---- */
/** binary → uppercase hex string (caller frees). data==NULL or len==0 returns "" (caller must still free). */
char *ThingModelHex_Encode(const uint8_t *data, size_t len);
/** hex string → binary. *out_data is malloc'd by this function, caller frees.
 *  hex may be upper/lower case; non-even length / contains non-hex characters → returns RC_DECODE_ERROR, *out_data=NULL, *out_len=0.
 *  hex==NULL or empty string → returns RC_OK, *out_data=NULL, *out_len=0. */
T_ThingModelReturnCode ThingModelHex_Decode(const char *hex, uint8_t **out_data, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_CODEC_H */

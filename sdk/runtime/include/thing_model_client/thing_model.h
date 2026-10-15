/**
 * @file    thing_model.h
 * @brief   Things-Model MQTT SDK master entry point — one include gets you the entire public API.
 *
 *   Usage:
 *     #include "thing_model_client/thing_model.h"
 *     /* then use ThingModelClientSession_Connect / etc. directly */
 *
 *   Equivalent to (the old way):
 *     #include "thing_model_client/thing_model_client_session.h"
 *     #include "thing_model_client/thing_model_client_types.h"
 *     #include "thing_model_client/thing_model_client_osal.h"
 *     #include "thing_model_client/thing_model_client_transport.h"
 *     #include "thing_model_client/thing_model_client_crypto.h"
 *
 *   Each capability header is still included separately:
 *     #include "flight_control/flight_control_client.h"
 */
#ifndef THING_MODEL_H
#define THING_MODEL_H

#include "thing_model_client_types.h"      /* version number + return codes + common types */
#include "thing_model_client_osal.h"       /* OSAL vtable */
#include "thing_model_client_crypto.h"     /* Crypto vtable */
#include "thing_model_client_transport.h"  /* Transport vtable */
#include "thing_model_client_session.h"    /* Session main API */

/* codec.h is the thin cJSON wrapper; application code usually does not use it directly,
 * #include "thing_model_client/thing_model_client_codec.h" manually when needed */

#endif /* THING_MODEL_H */

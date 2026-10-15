# ===========================================================================
#  edge-sdk —— Third-party dock integration SDK for the O4 ground station
#
#  Single top-level CMake, auto-detecting two forms:
#    • Release mode: sdk/generated/ snapshot present → pure static build, no codegen.
#    • Dev mode: no snapshot → include(internal/cmake/fetch_codegen.cmake) generates protocol code on the fly.
#
#  Build:
#    Dev mode:     cmake -B build   (see internal/ for the protocol code-generator setup)
#    Release mode: cmake -B build
#    Then:         cmake --build build -j
# ===========================================================================
cmake_minimum_required(VERSION 3.14)
project(edge_sdk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

option(EDGE_SDK_BUILD_TESTS "Build internal test targets (loopback CI demo)" ON)

# ---- Device selection: integrators that only ship ONE device can exclude the other entirely ----
#   dt5 = third-party dock  (domains: charging / config / flight)
#   dt7 = RID device        (domain:  surveillance)
#   The common core (session / discovery / id / auth) is always built.
#   Turning a device OFF drops both its src/*.c and its generated protocol code (dev mode) or
#   filters it out of the snapshot (release mode) — nothing of that device is compiled or linked.
option(EDGE_DOCK_ENABLE_DOCK "Build third-party dock device (dt5) domains" ON)
option(EDGE_DOCK_ENABLE_RID  "Build RID device (dt7) domains"              ON)
if(NOT EDGE_DOCK_ENABLE_DOCK AND NOT EDGE_DOCK_ENABLE_RID)
    message(FATAL_ERROR "[edge-sdk] at least one of EDGE_DOCK_ENABLE_DOCK / EDGE_DOCK_ENABLE_RID must be ON")
endif()
message(STATUS "[edge-sdk] devices: dock(dt5)=${EDGE_DOCK_ENABLE_DOCK} rid(dt7)=${EDGE_DOCK_ENABLE_RID}")

# Capability-domain proto stems owned by each device (used to select codegen input / filter snapshot).
set(_CORE_DOMAINS auth)                       # always built; common/tm are pulled in automatically
set(_DOCK_DOMAINS charging config flight)     # dt5
set(_RID_DOMAINS  surveillance)               # dt7

# ---- Credential config: config/dock_app_info.h.in -> build/dock_app_info.h ----
set(DOCK_APP_ID              "your_app_id"        CACHE STRING "DJI app id")
set(DOCK_APP_KEY             "your_app_key"       CACHE STRING "DJI app key")
set(DOCK_APP_LICENSE         "your_app_license"   CACHE STRING "DJI app license")
set(DOCK_APP_NAME            "your_app_name"      CACHE STRING "DJI app name")
set(DOCK_DEVICE_SN           "dock_sn_demo"       CACHE STRING "third-party dock (dt5) SN")
set(DOCK_RID_SN              ""                   CACHE STRING "RID (dt7) SN; empty = <DOCK_DEVICE_SN>_rid")
set(DOCK_BROKER_FALLBACK_IP  "192.168.50.10"      CACHE STRING "O4 broker fallback IP")
set(DOCK_BROKER_PORT         "1883"               CACHE STRING "O4 broker port")
# dt5 and dt7 are two distinct devices. The account "<app_id>_esdk_<sn>" is both the MQTT client_id
# and the topic owner segment, so the two must not share an SN: when they do, the one that connects
# SECOND is refused with CONNACK rc=5 (not authorised).
# DOCK_RID_DEVICE_SN is NOT a cache variable: it is recomputed on every configure so it cannot go
# stale when -DDOCK_DEVICE_SN changes.
if(NOT DOCK_RID_SN STREQUAL "")
    set(DOCK_RID_DEVICE_SN "${DOCK_RID_SN}")
elseif(NOT EDGE_DOCK_ENABLE_DOCK)
    # RID-only build: there is no dt5 in this binary to collide with, so DOCK_DEVICE_SN *is* the RID
    # device's SN. Without this branch a vendor shipping only an RID device would set
    # -DDOCK_DEVICE_SN=<their sn> (the intuitive knob) and get "<their sn>_rid" on the wire instead.
    set(DOCK_RID_DEVICE_SN "${DOCK_DEVICE_SN}")
else()
    set(DOCK_RID_DEVICE_SN "${DOCK_DEVICE_SN}_rid")
endif()
if(EDGE_DOCK_ENABLE_DOCK)
    message(STATUS "[edge-sdk] device SNs: dt5=${DOCK_DEVICE_SN} dt7=${DOCK_RID_DEVICE_SN}")
else()
    message(STATUS "[edge-sdk] device SN: dt7=${DOCK_RID_DEVICE_SN} (RID-only build)")
endif()
configure_file(config/dock_app_info.h.in
               ${CMAKE_BINARY_DIR}/generated_config/dock_app_info.h @ONLY)

# ---- Form detection ----
# sdk/ = generated protocol layer (snapshot): generated code + thing-model runtime. Named to match payload-sdk-iot.
set(_SNAPSHOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
if(EXISTS ${_SNAPSHOT_DIR}/generated)
    set(EDGE_SDK_RELEASE_MODE ON)
    message(STATUS "[edge-sdk] release mode: using sdk/ snapshot, no codegen")
else()
    set(EDGE_SDK_RELEASE_MODE OFF)
    message(STATUS "[edge-sdk] dev mode: codegen generated on the fly")
endif()

# ---- Dependencies (shared by both forms): cJSON bundled source; OpenSSL/mosquitto taken per target platform ----
#   host (default)          → system packages find_package/find_library.
#   cross-compile / explicit → bundled third_party/<triplet>/ self-contained mini-sysroot (headers + prebuilt .so),
#     -DEDGE_SDK_USE_VENDORED_DEPS=ON  the build never touches system headers/libs. See third_party/vendor-deps.sh for material.
#   Pre-fill OPENSSL_*/MOSQUITTO_* cache variables → release mode uses them directly; dev mode has the protocol runtime
#   auto-recognize (runtime/c sees all five variables pre-filled → goes self-contained, no longer find_package).
option(EDGE_SDK_USE_VENDORED_DEPS "Use bundled third_party OpenSSL/mosquitto instead of system packages" OFF)
# Statically link OpenSSL/mosquitto: the artifact no longer depends on these two .so files, no rpath needed (glibc still dynamic).
# Requires third_party/<triplet>/lib/lib{ssl,crypto,mosquitto}.a — generate with build-static-deps.sh.
# Implies the vendored path (static archives are only found in the bundled mini-sysroot).
option(EDGE_SDK_STATIC_DEPS "Statically link OpenSSL/mosquitto (.a), artifact has no .so dependency" OFF)
if(CMAKE_CROSSCOMPILING OR EDGE_SDK_USE_VENDORED_DEPS OR EDGE_SDK_STATIC_DEPS)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(TP_TRIPLET aarch64-linux-gnu)
    else()
        set(TP_TRIPLET x86_64-linux-gnu)
    endif()
    set(TP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${TP_TRIPLET})
    # Library suffix: static .a or dynamic .so
    if(EDGE_SDK_STATIC_DEPS)
        set(_L a)
    else()
        set(_L so)
    endif()
    if((NOT EXISTS ${TP_ROOT}/lib/libmosquitto.${_L}) OR
       (NOT EXISTS ${TP_ROOT}/lib/libssl.${_L}) OR
       (NOT EXISTS ${TP_ROOT}/lib/libcrypto.${_L}) OR
       (NOT EXISTS ${TP_ROOT}/include/openssl/opensslconf.h))
        if(EDGE_SDK_STATIC_DEPS)
            message(FATAL_ERROR
                "bundled static archives (.a) incomplete: ${TP_ROOT}/lib\n"
                "   generate first: bash ${CMAKE_CURRENT_SOURCE_DIR}/third_party/build-static-deps.sh ${TP_TRIPLET}")
        else()
            message(FATAL_ERROR
                "bundled third-party dependencies incomplete: ${TP_ROOT}\n"
                "   collect first: bash ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vendor-deps.sh\n"
                "   or switch to system packages (host only): drop toolchain / -DEDGE_SDK_USE_VENDORED_DEPS=OFF")
        endif()
    endif()
    message(STATUS "[edge-sdk] third_party self-contained: ${TP_ROOT} (link: ${_L})")
    # Pre-fill → bypass all system find_*; dev mode protocol runtime also reuses these five variables
    set(OPENSSL_INCLUDE_DIR    ${TP_ROOT}/include                CACHE PATH     "" FORCE)
    set(OPENSSL_SSL_LIBRARY    ${TP_ROOT}/lib/libssl.${_L}       CACHE FILEPATH "" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY ${TP_ROOT}/lib/libcrypto.${_L}    CACHE FILEPATH "" FORCE)
    set(MOSQUITTO_LIB          ${TP_ROOT}/lib/libmosquitto.${_L} CACHE FILEPATH "" FORCE)
    set(MOSQUITTO_INC          ${TP_ROOT}/include                CACHE PATH     "" FORCE)
    set(_OPENSSL_LINK ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
    set(EDGE_SDK_VENDORED_DEPS ON)
    if(EDGE_SDK_STATIC_DEPS)
        # System transitive deps of the openssl/mosquitto static archives (dlopen / clock / math); static needs no rpath
        set(_STATIC_DEP_EXTRA ${CMAKE_DL_LIBS} rt m)
    else()
        # Runtime .so loading: deployment tree bin/<demo> + lib/<.so> → $ORIGIN/../lib; build tree runs directly → third_party absolute path
        add_link_options(-Wl,--disable-new-dtags)
        set(CMAKE_BUILD_RPATH   "$ORIGIN/../lib;${TP_ROOT}/lib")
        set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
    endif()
else()
    find_package(OpenSSL REQUIRED)
    find_library(MOSQUITTO_LIB mosquitto REQUIRED)
    find_path(MOSQUITTO_INC mosquitto.h REQUIRED)
    set(_OPENSSL_LINK OpenSSL::SSL OpenSSL::Crypto)
endif()
# libuuid: optional. In cross mode, if not bundled → NOTFOUND, dock_id_provider.c uses the "SN-counter" fallback.
find_library(UUID_LIB uuid)
set(CJSON_SRC_FILE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON/cJSON.c)

# ---- Protocol code + runtime: take source per form ----
# Domains excluded by the device options — their generated code is filtered out (release) or never generated (dev).
set(_EXCLUDED_DOMAINS "")
if(NOT EDGE_DOCK_ENABLE_DOCK)
    list(APPEND _EXCLUDED_DOMAINS ${_DOCK_DOMAINS})
endif()
if(NOT EDGE_DOCK_ENABLE_RID)
    list(APPEND _EXCLUDED_DOMAINS ${_RID_DOMAINS})
endif()

if(EDGE_SDK_RELEASE_MODE)
    file(GLOB_RECURSE GEN_SRC     ${_SNAPSHOT_DIR}/generated/*.c)
    # Drop generated .c of excluded domains (snapshot lays each domain in its own subdir → path match).
    foreach(_dom ${_EXCLUDED_DOMAINS})
        list(FILTER GEN_SRC EXCLUDE REGEX "/${_dom}/")
    endforeach()
    file(GLOB         RUNTIME_SRC ${_SNAPSHOT_DIR}/runtime/src/*.c
                                  ${_SNAPSHOT_DIR}/runtime/src/osal/*.c)
    # edge is host-only: drop the rtos backend files if the snapshot carries them
    # (osal_freertos / transport_lwip / crypto_mbedtls need FreeRTOS/lwIP/mbedTLS headers).
    list(FILTER RUNTIME_SRC EXCLUDE REGEX "(osal_freertos|transport_lwip|crypto_mbedtls)")
    set(GEN_INC     ${_SNAPSHOT_DIR}/generated)
    set(RUNTIME_INC ${_SNAPSHOT_DIR}/runtime/include ${_SNAPSHOT_DIR}/runtime/src)
    set(_CODEGEN_DEPS "")
    if(EDGE_SDK_STATIC_DEPS)
        # Static archives depend on each other (mosquitto→ssl→crypto); use group to avoid ordering;
        # system transitive deps (dl/rt/m) are appended uniformly at the end of edge_dock, not duplicated here.
        set(_RUNTIME_LIBS
            -Wl,--start-group ${MOSQUITTO_LIB} ${_OPENSSL_LINK} -Wl,--end-group pthread)
    else()
        set(_RUNTIME_LIBS ${_OPENSSL_LINK} ${MOSQUITTO_LIB} pthread)
    endif()
    set(_NEED_CJSON_SRC ON)
else()
    set(CJSON_SRC ${CJSON_SRC_FILE} CACHE FILEPATH "vendored cJSON source" FORCE)
    set(CJSON_INC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON CACHE PATH "" FORCE)
    # host: the protocol runtime falls back to system packages; cross/vendored: the five variables were pre-filled above → runtime self-contained.
    if(NOT EDGE_SDK_VENDORED_DEPS)
        set(THING_MODEL_USE_SYSTEM_DEPS ON CACHE BOOL "" FORCE)
    endif()
    # edge is host platform + dock profile: pick the runtime composition before pulling in the code generator.
    set(THING_MODEL_PLATFORM host CACHE STRING "" FORCE)
    set(THING_MODEL_PROFILE  dock CACHE STRING "" FORCE)
    include(internal/cmake/fetch_codegen.cmake)

    # Select proto by enabled device: core domains always, dock/rid domains per option.
    # (Excluding a device means its proto is never fed to codegen → no generated code for it at all.)
    set(_WANTED_DOMAINS ${_CORE_DOMAINS})
    if(EDGE_DOCK_ENABLE_DOCK)
        list(APPEND _WANTED_DOMAINS ${_DOCK_DOMAINS})
    endif()
    if(EDGE_DOCK_ENABLE_RID)
        list(APPEND _WANTED_DOMAINS ${_RID_DOMAINS})
    endif()
    set(DOCK_PROTOS "")
    foreach(_dom ${_WANTED_DOMAINS})
        set(_p ${THING_MODEL_DIR}/proto/esdk_${_dom}.proto)
        if(NOT EXISTS ${_p})
            message(FATAL_ERROR "[edge-sdk] proto for domain '${_dom}' not found: ${_p}")
        endif()
        list(APPEND DOCK_PROTOS ${_p})
    endforeach()
    if(NOT DOCK_PROTOS)
        message(FATAL_ERROR "[edge-sdk] no esdk_*.proto selected in ${THING_MODEL_DIR}/proto")
    endif()
    message(STATUS "[edge-sdk] capability-domain proto: ${DOCK_PROTOS}")

    # Run client / server once each over all protos:
    #   uplink domains (charging/config) use the client artifact (dock_client),
    #   downlink domains (auth/flight/surveillance) use the server artifact (dock_impl/publisher).
    #   The other half of each domain is harmless dead code (never linked-in path). Artifacts go into independent subdirectories by proto stem, zero overlap.
    thing_model_generate(
        OUT_DIR ${CMAKE_BINARY_DIR}/_gen/dock TARGET esdk-dock ROLE client
        PROTO_FILES ${DOCK_PROTOS})
    set(_UP_TGT ${THING_MODEL_CODEGEN_TARGET})
    thing_model_generate(
        OUT_DIR ${CMAKE_BINARY_DIR}/_gen/dock TARGET esdk-dock ROLE server
        PROTO_FILES ${DOCK_PROTOS})
    set(_DOWN_TGT ${THING_MODEL_CODEGEN_TARGET})

    file(GLOB_RECURSE GEN_SRC ${CMAKE_BINARY_DIR}/_gen/dock/*.c)
    set(GEN_INC       ${CMAKE_BINARY_DIR}/_gen/dock)
    set(RUNTIME_SRC   "")
    set(RUNTIME_INC   "")
    set(_CODEGEN_DEPS ${_UP_TGT} ${_DOWN_TGT})
    set(_RUNTIME_LIBS
        -Wl,--whole-archive
        thing_model_client_transport_mosquitto thing_model_client_crypto_openssl
        -Wl,--no-whole-archive
        thing_model_client_runtime pthread)
    set(_NEED_CJSON_SRC OFF)
endif()

# ---- SDK library libedge_dock ----
# Core is always compiled; dock/rid src dirs are added only for enabled devices.
file(GLOB LIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c)
if(EDGE_DOCK_ENABLE_DOCK)
    file(GLOB _DOCK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/dock/*.c)
    list(APPEND LIB_SRC ${_DOCK_SRC})
endif()
if(EDGE_DOCK_ENABLE_RID)
    file(GLOB _RID_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/rid/*.c)
    list(APPEND LIB_SRC ${_RID_SRC})
endif()
add_library(edge_dock STATIC ${LIB_SRC} ${GEN_SRC} ${RUNTIME_SRC})
# src/core holds internal-only headers (e.g. dock_color.h) shared by dock/rid domain sources.
target_include_directories(edge_dock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
if(_NEED_CJSON_SRC)
    target_sources(edge_dock PRIVATE ${CJSON_SRC_FILE})
endif()
# edge always uses the dock profile (ESDK2 plaintext access + dock envelope). Required in release mode
# where edge_dock compiles the runtime snapshot sources directly (dev mode also gets it via the
# thing_model_client_runtime PUBLIC define, harmless to set twice).
target_compile_definitions(edge_dock PRIVATE THING_MODEL_PROFILE_DOCK)
target_include_directories(edge_dock PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GEN_INC}
    ${RUNTIME_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
    ${CMAKE_BINARY_DIR}/generated_config
    ${MOSQUITTO_INC})
target_link_libraries(edge_dock PUBLIC ${_RUNTIME_LIBS})
# System transitive deps of static archives go last (dl/rt/m must come after the .a files that reference them)
if(_STATIC_DEP_EXTRA)
    target_link_libraries(edge_dock PUBLIC ${_STATIC_DEP_EXTRA})
endif()
if(UUID_LIB)
    target_compile_definitions(edge_dock PRIVATE DOCK_HAVE_LIBUUID=1)
    target_link_libraries(edge_dock PUBLIC ${UUID_LIB})
endif()
if(_CODEGEN_DEPS)
    add_dependencies(edge_dock ${_CODEGEN_DEPS})
endif()

# Release mode: the default transport/crypto impls (transport_mosq.o / crypto_openssl.o) are compiled
# INTO libedge_dock.a and are referenced only *weakly* by the session (thing_model_client_session.c
# resolves ThingModelTransport_Mosquitto / ThingModelCrypto_OpenSSL via weak symbols). GNU ld never
# pulls an archive member to satisfy a weak reference, so without help those factories link to NULL and
# every session fails at runtime with "no transport vtable". Force the two members in with -u (the dev
# build achieves the same by --whole-archive'ing the separate impl libs). INTERFACE → applies when any
# executable links edge_dock; the -u flags precede libedge_dock.a on the link line, so ld extracts them.
if(EDGE_SDK_RELEASE_MODE)
    target_link_options(edge_dock INTERFACE
        "LINKER:-u,ThingModelTransport_Mosquitto"
        "LINKER:-u,ThingModelCrypto_OpenSSL")
endif()

# ---- samples ----
# quickstart + the single-device samples build only when their device is enabled;
# the full-feature cli test bench needs both devices.
if(EDGE_DOCK_ENABLE_DOCK)
    add_executable(dock_quickstart samples/quickstart/main.c)
    target_link_libraries(dock_quickstart PRIVATE edge_dock)

    add_executable(dock_sample samples/dock/main.c)
    target_link_libraries(dock_sample PRIVATE edge_dock)
endif()

if(EDGE_DOCK_ENABLE_RID)
    add_executable(rid_sample samples/rid/main.c)
    target_link_libraries(rid_sample PRIVATE edge_dock)
endif()

if(EDGE_DOCK_ENABLE_DOCK AND EDGE_DOCK_ENABLE_RID)
    add_executable(dock_cli samples/cli/main.c)
    target_link_libraries(dock_cli PRIVATE edge_dock)
endif()

# ---- Internal test targets (release mode naturally lacks internal/ → auto-skipped) ----
# ci_e2e exercises both devices' domains → only built when both are enabled.
if(EDGE_SDK_BUILD_TESTS AND EDGE_DOCK_ENABLE_DOCK AND EDGE_DOCK_ENABLE_RID
   AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/internal/test/mock/ci_loopback_transport.c)
    add_executable(dock_ci internal/samples/ci_e2e/main.c internal/test/mock/ci_loopback_transport.c)
    target_link_libraries(dock_ci PRIVATE edge_dock)
endif()

# dock_ecode: MQTT access error-code bench. Same internal-only treatment as dock_ci above —
# guarded on internal/ existing, so a release tree has no such target. It links ${MOSQUITTO_LIB}
# directly because it bypasses the SDK transport. Rationale lives with the source, not here:
# CMakeLists.txt ships in the release package and internal/ does not.
if(EDGE_SDK_BUILD_TESTS
   AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/internal/samples/ecode/main.c)
    add_executable(dock_ecode internal/samples/ecode/main.c internal/samples/ecode/ecode_connack.c)
    target_link_libraries(dock_ecode PRIVATE edge_dock ${MOSQUITTO_LIB})
endif()

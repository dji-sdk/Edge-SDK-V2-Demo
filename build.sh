#!/usr/bin/env bash
# ===========================================================================
#  build.sh —— convenient build wrapper for edge-sdk (host / aarch64 cross)
#
#  Usage:
#    ./build.sh                build for host (x86_64, system OpenSSL/mosquitto) → build/
#    ./build.sh --host         same as above (explicit)
#    ./build.sh --cross        cross-compile aarch64 (bundled third_party/aarch64-linux-gnu) → build-aarch64/
#    ./build.sh -c             clean the corresponding build dir and reconfigure
#    ./build.sh -j 8           set concurrency
#    ./build.sh -h             help
#
#  This is the release build wrapper: the package ships a self-contained sdk/ snapshot,
#  so the build needs no code generator, no python, and no network access — just cmake && make.
#
#  Artifacts (host→build/, cross→build-aarch64/, the two dirs coexist independently, caches never cross):
#    build/dock_quickstart          # host: ELF ... x86-64
#    build-aarch64/dock_quickstart  # cross: ELF ... ARM aarch64
# ===========================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR=""          # determined by target (see below), host=build/ cross=build-aarch64/
JOBS="$(nproc 2>/dev/null || echo 4)"
TARGET="host"                        # host | cross
DO_CLEAN=0

usage() { sed -n '3,18p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)   usage; exit 0 ;;
        --host)      TARGET="host";  shift ;;
        --cross)     TARGET="cross"; shift ;;
        -c|--clean)  DO_CLEAN=1; shift ;;
        -j)          JOBS="$2"; shift 2 ;;
        -j*)         JOBS="${1#-j}"; shift ;;
        *) echo "unknown argument: $1 (use -h for usage)" >&2; exit 2 ;;
    esac
done

CMAKE_ARGS=()
# host and cross use **independent** build dirs: avoids a shared CMakeCache causing the toolchain to not take effect
# (CMake ignores a newly-passed CMAKE_TOOLCHAIN_FILE for an already-configured build tree, reusing the old compiler → architecture cross-contamination).
if [ "$TARGET" = "cross" ]; then
    BUILD_DIR="$SCRIPT_DIR/build-aarch64"
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$SCRIPT_DIR/cmake/toolchain-aarch64.cmake")
else
    BUILD_DIR="$SCRIPT_DIR/build"
fi

# Safeguard: if the target build tree already exists but the cached architecture does not match this target → force reconfigure, preventing cache cross-contamination.
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    _cached_cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
    case "$TARGET:$_cached_cc" in
        cross:*aarch64*) : ;;                 # expected cross, cache is also cross → OK
        host:*aarch64*)  DO_CLEAN=1 ;;         # expected host, cache is cross → reconfigure
        cross:*)         DO_CLEAN=1 ;;          # expected cross, cache is not cross → reconfigure
        *) : ;;
    esac
    [ "$DO_CLEAN" = 1 ] && echo "[guard] $BUILD_DIR cached architecture does not match target → auto clean and reconfigure"
fi

if [ "$DO_CLEAN" = 1 ]; then
    echo "[clean] rm -rf $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# ---- host mosquitto dev-shim: "just cmake && make" without needing libmosquitto-dev ----
# CMake's find_library(mosquitto)/find_path(mosquitto.h) need the *dev* package (libmosquitto.so symlink + header).
# Some hosts only have the *runtime* (libmosquitto.so.N) installed. Rather than require sudo apt-get install
# libmosquitto-dev, synthesize a minimal dev tree from the runtime .so + the vendored header, and hand it to
# cmake via -DMOSQUITTO_LIB/-DMOSQUITTO_INC. No-op when: a real dev package is present, the user already exported
# MOSQUITTO_LIB, or we're cross-building (cross uses the bundled third_party sysroot).
if [ "$TARGET" = "host" ] && [ -z "${MOSQUITTO_LIB:-}" ]; then
    # dev package present? = a bare libmosquitto.so (no version suffix) resolvable by the linker.
    _HAVE_DEV=0
    for _d in /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/local/lib; do
        [ -e "$_d/libmosquitto.so" ] && { _HAVE_DEV=1; break; }
    done
    if [ "$_HAVE_DEV" = 0 ]; then
        _RUNTIME_SO="$( { ldconfig -p 2>/dev/null || true; } | sed -n 's/.*=> //p' | grep 'libmosquitto\.so\.' | head -n1 || true)"
        _VENDOR_HDR="$SCRIPT_DIR/third_party/aarch64-linux-gnu/include/mosquitto.h"  # arch-independent API header
        if [ -n "$_RUNTIME_SO" ] && [ -f "$_VENDOR_HDR" ]; then
            _SHIM="$BUILD_DIR/.mosq_devshim"
            mkdir -p "$_SHIM/lib" "$_SHIM/include"
            ln -sf "$_RUNTIME_SO" "$_SHIM/lib/libmosquitto.so"
            cp -f "$_VENDOR_HDR" "$_SHIM/include/mosquitto.h"
            export MOSQUITTO_LIB="$_SHIM/lib/libmosquitto.so"
            export MOSQUITTO_INC="$_SHIM/include"
            echo "[mosq-shim] no libmosquitto-dev found → synthesized dev tree from $_RUNTIME_SO"
        else
            echo "[mosq-shim] WARN: no libmosquitto-dev and could not synthesize a shim" >&2
            echo "           install it:  sudo apt-get install -y libmosquitto-dev" >&2
        fi
    fi
fi
# Forward the shim (or a user-exported override) to cmake.
if [ -n "${MOSQUITTO_LIB:-}" ]; then
    CMAKE_ARGS+=("-DMOSQUITTO_LIB=$MOSQUITTO_LIB")
    [ -n "${MOSQUITTO_INC:-}" ] && CMAKE_ARGS+=("-DMOSQUITTO_INC=$MOSQUITTO_INC")
fi

echo "[cmake] configure ($TARGET) → $BUILD_DIR ${CMAKE_ARGS[*]:-}"
# ${arr[@]+"${arr[@]}"} = portable "expand only if non-empty": a plain "${CMAKE_ARGS[@]}" trips
# `set -u` ("unbound variable") on bash < 4.4 when the array is empty (host build, mosquitto dev pkg present).
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" ${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}

echo "[build] -j $JOBS"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo
echo "==== artifacts ===="
for b in dock_quickstart dock_cli; do
    bin="$BUILD_DIR/$b"
    [ -f "$bin" ] || continue
    echo "  $bin"
    info="$(file "$bin" 2>/dev/null | sed 's/^[^:]*: //' | cut -d, -f1-2)" && echo "      $info"
done

#!/usr/bin/env bash
# ============================================================
# third_party/build-static-deps.sh — build OpenSSL/mosquitto static
#   archives (.a) + headers for a given triplet, landing them under
#   third_party/<triplet>/{lib,include}, for use with
#   cmake -DEDGE_SDK_STATIC_DEPS=ON static linking (the output has no .so
#   dependencies, glibc is still dynamic).
#
# Usage:
#   bash third_party/build-static-deps.sh x86_64-linux-gnu     # host
#   bash third_party/build-static-deps.sh aarch64-linux-gnu    # cross (needs aarch64-linux-gnu-gcc)
#
# Source acquisition (by priority, since different environments have different network policies):
#   1) Environment variable pointing to an already-extracted source directory:
#        OPENSSL_SRC=/path/to/openssl-1.1.1w  MOSQUITTO_SRC=/path/to/mosquitto-1.6.9
#   2) Environment variable pointing to a tarball:
#        OPENSSL_TARBALL=/path/openssl-1.1.1w.tar.gz  MOSQUITTO_TARBALL=/path/mosquitto-1.6.9.tar.gz
#   3) Download from URL (can be overridden with an internal mirror):
#        OPENSSL_URL=...   MOSQUITTO_URL=...
#   For host OpenSSL, if libssl-dev is already installed on the system (libssl.a/libcrypto.a exist)
#   -> reuse it directly, skip the build.
#
# Version pinning: OpenSSL 1.1.1w / mosquitto 1.6.9 (aligned with the vendored .so).
# ============================================================
set -euo pipefail

TRIPLET="${1:-}"
case "$TRIPLET" in
    x86_64-linux-gnu|aarch64-linux-gnu) ;;
    *) echo "usage: $0 <x86_64-linux-gnu|aarch64-linux-gnu>" >&2; exit 2 ;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DST="$HERE/$TRIPLET"; LIBDIR="$DST/lib"; INCDIR="$DST/include"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
JOBS="$(nproc 2>/dev/null || echo 4)"
mkdir -p "$LIBDIR" "$INCDIR"

OPENSSL_VER=1.1.1w
MOSQUITTO_VER=1.6.9
OPENSSL_URL="${OPENSSL_URL:-https://www.openssl.org/source/openssl-${OPENSSL_VER}.tar.gz}"
MOSQUITTO_URL="${MOSQUITTO_URL:-https://mosquitto.org/files/source/mosquitto-${MOSQUITTO_VER}.tar.gz}"

# -- cross toolchain --
if [ "$TRIPLET" = aarch64-linux-gnu ]; then
    CROSS=aarch64-linux-gnu-
    CC="${CC:-$(command -v aarch64-linux-gnu-gcc-11 || command -v aarch64-linux-gnu-gcc-10 \
        || command -v aarch64-linux-gnu-gcc-9 || command -v aarch64-linux-gnu-gcc)}"
    [ -n "$CC" ] || { echo "aarch64-linux-gnu-gcc not found (apt install gcc-aarch64-linux-gnu)" >&2; exit 1; }
    OSSL_TARGET=linux-aarch64
else
    CROSS=""; CC="${CC:-gcc}"; OSSL_TARGET=linux-x86_64
fi
echo "[build-static-deps] triplet=$TRIPLET  CC=$CC"

# -- fetch source: directory > tarball > URL --
fetch_src() {  # name ver url srcvar tarvar -> echo source directory
    local name="$1" ver="$2" url="$3" srcenv="$4" tarenv="$5" d="${!4:-}" t="${!5:-}"
    if [ -n "$d" ]; then echo "$d"; return; fi
    if [ -z "$t" ]; then
        t="$WORK/$name-$ver.tar.gz"
        echo "  downloading $url" >&2
        curl -fSL --max-time 120 -o "$t" "$url" \
            || { echo "download failed. Preset ${srcenv}=extracted-dir or ${tarenv}=tarball (see script header)" >&2; exit 1; }
    fi
    tar -C "$WORK" -xzf "$t"
    echo "$WORK/$name-$ver"
}

# -- OpenSSL: host prefers reusing the system .a; otherwise build from source with no-shared --
build_openssl() {
    local sys_ssl=/usr/lib/$TRIPLET/libssl.a sys_cr=/usr/lib/$TRIPLET/libcrypto.a
    if [ "$TRIPLET" = x86_64-linux-gnu ] && [ -f "$sys_ssl" ] && [ -f "$sys_cr" ]; then
        echo "[openssl] reusing system static libraries"
        cp -f "$sys_ssl" "$sys_cr" "$LIBDIR/"
        [ -d "$INCDIR/openssl" ] || { mkdir -p "$INCDIR/openssl"; cp -f /usr/include/openssl/*.h "$INCDIR/openssl/"; }
        [ -f "$INCDIR/openssl/opensslconf.h" ] || cp -f /usr/include/$TRIPLET/openssl/opensslconf.h "$INCDIR/openssl/"
        return
    fi
    echo "[openssl] building from source ($OSSL_TARGET, no-shared)"
    local src; src="$(fetch_src openssl "$OPENSSL_VER" "$OPENSSL_URL" OPENSSL_SRC OPENSSL_TARBALL)"
    ( cd "$src"
      ./Configure "$OSSL_TARGET" no-shared no-tests --cross-compile-prefix="$CROSS" >/dev/null
      make -j"$JOBS" >/dev/null )
    cp -f "$src/libssl.a" "$src/libcrypto.a" "$LIBDIR/"
    mkdir -p "$INCDIR/openssl"; cp -f "$src"/include/openssl/*.h "$INCDIR/openssl/"
}

# -- mosquitto: every environment needs a static build from source (apt does not provide .a) --
build_mosquitto() {
    echo "[mosquitto] building static library from source"
    local src; src="$(fetch_src mosquitto "$MOSQUITTO_VER" "$MOSQUITTO_URL" MOSQUITTO_SRC MOSQUITTO_TARBALL)"
    # build only libmosquitto static: disable shared/broker/client/docs, TLS uses the openssl headers/libs produced above
    ( cd "$src/lib"
      make -j"$JOBS" \
        CC="$CC" AR="${CROSS}ar" \
        CFLAGS="-I$INCDIR -DWITH_TLS -DWITH_THREADING -fPIC" \
        libmosquitto.a >/dev/null 2>&1 \
      || make -j"$JOBS" CC="$CC" AR="${CROSS}ar" \
        CFLAGS="-I$INCDIR -DWITH_TLS -DWITH_THREADING -fPIC" libmosquitto.a )
    cp -f "$src/lib/libmosquitto.a" "$LIBDIR/"
    cp -f "$src/lib/mosquitto.h"    "$INCDIR/"
}

build_openssl
build_mosquitto

echo
echo "[build-static-deps] done -> $LIBDIR"
ls -l "$LIBDIR"/*.a
echo "you can now run: cmake -B build -DEDGE_SDK_STATIC_DEPS=ON $([ "$TRIPLET" = aarch64-linux-gnu ] && echo -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake)"

#!/usr/bin/env bash
# ============================================================
# third_party/vendor-deps.sh — collect/refresh vendored third-party deps (headers + prebuilt .so)
#
# edge-sdk dependency policy:
#   - host (x86_64) uses system packages find_package(OpenSSL)/find_library(mosquitto) by default,
#     no vendored .so needed; only when you explicitly set -DEDGE_SDK_USE_VENDORED_DEPS=ON does it
#     use the x86_64 vendored deps.
#   - aarch64 (cross target) uses the self-contained mini-sysroot vendored at
#     third_party/aarch64-linux-gnu; its .so files are target-platform prebuilt artifacts (cannot be
#     regenerated on an x86 host) and are already committed to the repo.
#   - libuuid is not vendored: on aarch64 cross builds UUID_LIB=NOTFOUND, and
#     src/dock_id_provider.c falls back to "SN-counter" (see its #if DOCK_HAVE_LIBUUID).
#
# Directory convention (one directory per GNU triplet, each self-contained):
#   third_party/<triplet>/include/openssl/*.h   OpenSSL 1.1.1 public headers + this arch's opensslconf.h
#   third_party/<triplet>/include/mosquitto.h   libmosquitto header
#   third_party/<triplet>/lib/lib{ssl,crypto,mosquitto}.so*
#   triplet in { x86_64-linux-gnu, aarch64-linux-gnu }
#
# Version pinning: OpenSSL 1.1.1 / libmosquitto 1.6.9 (aligned with the vendored aarch64 .so).
#
# What this script does:
#   - Refresh x86_64 (from system packages, needs libssl-dev libmosquitto-dev) -- escape hatch only.
#   - Add/replace the OpenSSL public headers for aarch64 (cross-arch generic, only opensslconf.h
#     takes the arm64 version); the aarch64 .so stays vendored and is not regenerated here.
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SYS_OPENSSL_INC="${SYS_OPENSSL_INC:-/usr/include/openssl}"
SYS_OPENSSLCONF_X86="${SYS_OPENSSLCONF_X86:-/usr/include/x86_64-linux-gnu/openssl/opensslconf.h}"
SYS_OPENSSLCONF_ARM="${SYS_OPENSSLCONF_ARM:-/usr/include/aarch64-linux-gnu/openssl/opensslconf.h}"
SYS_MOSQUITTO_H="${SYS_MOSQUITTO_H:-/usr/include/mosquitto.h}"
SYS_LIBDIR_X86="${SYS_LIBDIR_X86:-/usr/lib/x86_64-linux-gnu}"

die(){ echo "ERROR: $*" >&2; exit 1; }

copy_openssl_headers() {
    local dst="$1" conf="$2"
    [ -d "$SYS_OPENSSL_INC" ] || die "OpenSSL header directory not found: $SYS_OPENSSL_INC (install libssl-dev)"
    [ -f "$conf" ]            || die "opensslconf.h not found: $conf"
    mkdir -p "$dst/openssl"
    cp -f "$SYS_OPENSSL_INC"/*.h "$dst/openssl/"
    cp -f "$conf" "$dst/openssl/opensslconf.h"
    echo "  openssl headers -> $dst/openssl ($(ls "$dst/openssl" | wc -l) files, conf=$conf)"
}

echo "[vendor-deps] OpenSSL 1.1.1 / libmosquitto 1.6.9"

# -- x86_64-linux-gnu (only for the -DEDGE_SDK_USE_VENDORED_DEPS=ON escape hatch; host uses system packages by default) --
X86="$HERE/x86_64-linux-gnu"
echo "[x86_64-linux-gnu]"
copy_openssl_headers "$X86/include" "$SYS_OPENSSLCONF_X86"
[ -f "$SYS_MOSQUITTO_H" ] || die "mosquitto.h not found: $SYS_MOSQUITTO_H (install libmosquitto-dev)"
cp -f "$SYS_MOSQUITTO_H" "$X86/include/mosquitto.h"
echo "  mosquitto.h -> $X86/include/mosquitto.h"
mkdir -p "$X86/lib"
for real in libssl.so.1.1 libcrypto.so.1.1 libmosquitto.so.1.6.9; do
    [ -f "$SYS_LIBDIR_X86/$real" ] || die "not found: $SYS_LIBDIR_X86/$real"
    cp -f "$SYS_LIBDIR_X86/$real" "$X86/lib/$real"
done
( cd "$X86/lib"
  ln -sf libssl.so.1.1         libssl.so
  ln -sf libcrypto.so.1.1      libcrypto.so
  ln -sf libmosquitto.so.1.6.9 libmosquitto.so.1
  ln -sf libmosquitto.so.1.6.9 libmosquitto.so )
echo "  libs -> $X86/lib ($(ls "$X86/lib" | wc -l) entries)"

# -- aarch64-linux-gnu (target board: .so already vendored, only add/replace OpenSSL headers) --
ARM="$HERE/aarch64-linux-gnu"
echo "[aarch64-linux-gnu]"
if [ ! -f "$SYS_OPENSSLCONF_ARM" ]; then
    echo "  WARN: arm64 opensslconf.h not found ($SYS_OPENSSLCONF_ARM)."
    echo "        Install libssl-dev:arm64, or specify -e SYS_OPENSSLCONF_ARM=/path and rerun; existing headers are kept."
else
    copy_openssl_headers "$ARM/include" "$SYS_OPENSSLCONF_ARM"
fi
[ -f "$ARM/include/mosquitto.h" ] && echo "  mosquitto.h: already vendored (kept)"
[ -f "$ARM/lib/libmosquitto.so" ] && echo "  libs: aarch64 prebuilt .so already vendored (kept, cannot be regenerated on host)"

echo "[vendor-deps] done."

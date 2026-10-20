#!/bin/sh
# Fetch the x86_64 VVEye client SDK and prepare its native library directory.
# The SDK is proprietary; this script downloads it from VVEye's public SDK
# page rather than redistributing it in this repository.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VENDOR=${VVEYE_SDK_DIR:-"$ROOT/vendor"}
SDK_VERSION=${VVEYE_SDK_VERSION:-v106}
URL=${VVEYE_LIBT2U_URL:-"http://www.vveye.com/download/t2u/libt2u_android_${SDK_VERSION}.7z"}
FORCE_DOWNLOAD=${VVEYE_FORCE_DOWNLOAD:-0}
ARCHIVE_SHA256=${VVEYE_ARCHIVE_SHA256:-e81df734b08b5c9b5dc55cffbfc01b1e1634ef35067dde47bab66c983c9ef279}
LIB_SHA256=${VVEYE_LIB_SHA256:-fd5e265ba44bb141efa8c3f6c2ee6f3ad7bb1f5346882dc9c0c573bdd942732f}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/vveye-p2p-client-sdk.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: required command not found: $1" >&2
        exit 1
    }
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        echo "error: no SHA-256 tool found (sha256sum, shasum, or openssl)" >&2
        return 1
    fi
}

verify_sha256() {
    file=$1
    expected=$2
    label=$3
    actual=$(sha256_of "$file") || exit 1
    if [ "$actual" != "$expected" ]; then
        echo "error: $label checksum mismatch" >&2
        echo "  file:     $file" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
    echo "$label checksum OK"
}

if [ -f "$VENDOR/libt2u.so" ] && [ "$FORCE_DOWNLOAD" != 1 ]; then
    echo "using existing vendor library: $VENDOR/libt2u.so"
else
    need curl

    if command -v bsdtar >/dev/null 2>&1; then
        EXTRACT="bsdtar"
    elif command -v 7z >/dev/null 2>&1; then
        EXTRACT="7z"
    elif command -v 7zz >/dev/null 2>&1; then
        EXTRACT="7zz"
    else
        echo "error: install bsdtar or 7z to extract the SDK" >&2
        exit 1
    fi

    echo "VVEye Android client SDK $SDK_VERSION"
    echo "downloading VVEye SDK: $URL"
    curl --fail --location --retry 3 --output "$TMP/sdk.7z" "$URL"
    verify_sha256 "$TMP/sdk.7z" "$ARCHIVE_SHA256" "SDK archive"

    mkdir -p "$TMP/unpacked" "$VENDOR"
    case "$EXTRACT" in
        bsdtar) bsdtar -xf "$TMP/sdk.7z" -C "$TMP/unpacked" ;;
        *) "$EXTRACT" x -y "-o$TMP/unpacked" "$TMP/sdk.7z" >/dev/null ;;
    esac

    LIB=$(find "$TMP/unpacked" -type f -path '*/x86_64/libt2u.so' -print -quit)
    HEADER=$(find "$TMP/unpacked" -type f -name 'libt2u.h' -print -quit)
    if [ -z "$LIB" ]; then
        echo "error: the downloaded archive has no x86_64/libt2u.so" >&2
        exit 1
    fi
    if [ -z "$HEADER" ]; then
        echo "error: the downloaded archive has no libt2u.h" >&2
        exit 1
    fi
    verify_sha256 "$LIB" "$LIB_SHA256" "SDK library"
    install -m 0755 "$LIB" "$VENDOR/libt2u.so"
    install -m 0644 "$HEADER" "$ROOT/include/libt2u.h"
fi

verify_sha256 "$VENDOR/libt2u.so" "$LIB_SHA256" "libt2u.so"

# The SDK's Android NEEDED entries (libc.so, libm.so, ...) are aliased to the
# system runtime ELF objects by the Makefile, under build/.  This directory
# holds only the vendor library itself.

cat <<EOF
SDK installed in: $VENDOR
Build the shim and CLI with: make
Run: ./vveye-p2p-client --server HOST --uid UID --remote-port REMOTE --local-port LOCAL
EOF

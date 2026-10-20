#!/bin/bash
# Builds the WebRTC data channel library (libdatachannel, with mbedTLS) as static libraries for
# the server targets that get WebRTC for web players: /deps/<target>/{include,lib}.
#   build_deps.sh <target> [toolchain file]      (run inside the build image)
# Pinned versions: rebuilt only when this file changes (the image caches it).
set -euo pipefail

TARGET="$1"
TOOLCHAIN="${2:-}"
MBEDTLS_TAG=v3.6.2
DATACHANNEL_TAG=v0.22.6
PREFIX="/deps/$TARGET"
WORK="/tmp/deps-$TARGET"
TC=()
[ -n "$TOOLCHAIN" ] && TC=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN")

mkdir -p "$WORK" "$PREFIX/lib" "$PREFIX/include"
cd "$WORK"

# mbedTLS
[ -d mbedtls ] || git clone -q --depth 1 --branch "$MBEDTLS_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/Mbed-TLS/mbedtls.git
# libdatachannel needs DTLS-SRTP (off in mbedTLS's default config)
sed -i 's|^//#define MBEDTLS_SSL_DTLS_SRTP|#define MBEDTLS_SSL_DTLS_SRTP|' mbedtls/include/mbedtls/mbedtls_config.h
grep -q '^#define MBEDTLS_SSL_DTLS_SRTP' mbedtls/include/mbedtls/mbedtls_config.h
cmake -S mbedtls -B mbedtls-build -G Ninja -DCMAKE_BUILD_TYPE=Release "${TC[@]}" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$PREFIX" > mbedtls.log
cmake --build mbedtls-build -j "$(nproc)" >> mbedtls.log
cmake --install mbedtls-build >> mbedtls.log

# libdatachannel (data channels only: no media, no WebSocket), its libjuice / usrsctp / plog
[ -d libdatachannel ] || git clone -q --depth 1 --branch "$DATACHANNEL_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/paullouisageneau/libdatachannel.git
cmake -S libdatachannel -B ldc-build -G Ninja -DCMAKE_BUILD_TYPE=Release "${TC[@]}" \
    -DUSE_MBEDTLS=ON -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DMbedTLS_INCLUDE_DIR="$PREFIX/include" -DMbedTLS_LIBRARY="$PREFIX/lib/libmbedtls.a" \
    -DMbedCrypto_LIBRARY="$PREFIX/lib/libmbedcrypto.a" -DMbedX509_LIBRARY="$PREFIX/lib/libmbedx509.a" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON > ldc.log || { tail -30 ldc.log; exit 1; }
cmake --build ldc-build --target datachannel-static -j "$(nproc)" >> ldc.log || { grep -E "error" ldc.log | head -20; exit 1; }

cp -r libdatachannel/include/rtc "$PREFIX/include/"
find ldc-build -name 'libdatachannel-static.a' -exec cp {} "$PREFIX/lib/libdatachannel.a" \;
find ldc-build -name 'libjuice-static.a' -exec cp {} "$PREFIX/lib/libjuice.a" \;
find ldc-build -name 'libusrsctp.a' -exec cp {} "$PREFIX/lib/" \;
ls -la "$PREFIX/lib"
rm -rf "$WORK"

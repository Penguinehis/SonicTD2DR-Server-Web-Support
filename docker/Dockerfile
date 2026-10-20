# Cross-compiling build environment for betterserver.
# Runs on any x86_64 Docker host and produces every supported target:
#
#   Windows  x64 / x86            mingw-w64  (GCC)
#   Windows  arm64 / arm          llvm-mingw (Clang; Debian's mingw has no ARM)
#   Linux    x64 / x86            GCC
#   Linux    arm64 / arm          GCC cross toolchains
#
# Every shipped binary is statically linked, so nothing has to be installed on
# the machines that run them.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# arm64 libraries come from multiarch, so the cross build gets sqlite too.
RUN dpkg --add-architecture arm64

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        libsqlite3-dev \
        libsqlite3-dev:arm64 \
        cmake \
        ninja-build \
        mingw-w64 \
        crossbuild-essential-arm64 \
        crossbuild-essential-armhf \
        crossbuild-essential-i386 \
        ca-certificates \
        curl \
        xz-utils \
        file \
        git \
    && rm -rf /var/lib/apt/lists/*

# mingw's default "win32" thread model has no pthreads and no C11 <threads.h>.
# The posix variant provides both, which this codebase needs.
RUN for p in x86_64-w64-mingw32 i686-w64-mingw32; do \
        update-alternatives --set $p-gcc /usr/bin/$p-gcc-posix; \
        update-alternatives --set $p-g++ /usr/bin/$p-g++-posix; \
    done

# llvm-mingw adds the aarch64/armv7 Windows targets that mingw-w64 lacks.
# Pinned so the image is reproducible.
ARG LLVM_MINGW_VERSION=20260922
RUN curl -fsSL -o /tmp/llvm-mingw.tar.xz \
        "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz" \
    && mkdir -p /opt/llvm-mingw \
    && tar -xf /tmp/llvm-mingw.tar.xz -C /opt/llvm-mingw --strip-components=1 \
    && rm /tmp/llvm-mingw.tar.xz

ENV PATH="/opt/llvm-mingw/bin:${PATH}"

# WebRTC data channels for web players: libdatachannel + mbedTLS, static, for the targets the
# hosting nodes and most hosts use (others build without WebRTC: WebSocket only)
COPY build_deps.sh toolchain-aarch64.cmake toolchain-mingw64.cmake /deps-src/
RUN bash /deps-src/build_deps.sh linux-x64 \
 && bash /deps-src/build_deps.sh linux-arm64 /deps-src/toolchain-aarch64.cmake \
 && bash /deps-src/build_deps.sh windows-x64 /deps-src/toolchain-mingw64.cmake

WORKDIR /src

#!/usr/bin/env bash
# Runs INSIDE the betterserver-build container.
# Builds the requested targets from /src and drops the artifacts in /out.
set -euo pipefail

SRC=/src
OUT=/out
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="$(nproc)"

ALL_TARGETS=(windows-x64 windows-x86 windows-arm64 windows-arm
             linux-x64 linux-x86 linux-arm64 linux-arm)

targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
    targets=(windows-x64 linux-arm64)
fi

# "all" anywhere in the list expands to every target.
expanded=()
for t in "${targets[@]}"; do
    if [ "$t" = "all" ]; then
        expanded+=("${ALL_TARGETS[@]}")
    else
        expanded+=("$t")
    fi
done
targets=("${expanded[@]}")

failed=()

build_one() {
    local name="$1" toolchain="$2" artifact="$3" outname="$4"
    shift 4
    local extra=("$@")
    local dir="/build/$name"
    # WebRTC for web players where the image has the library for this target
    [ -d "/deps/$name/lib" ] && extra+=(-DWEBRTC_DEPS="/deps/$name")

    echo ""
    echo "=============================================================="
    echo "  Building $name  ($BUILD_TYPE)"
    echo "=============================================================="

    mkdir -p /build
    rm -rf "$dir"
    if ! cmake -S "$SRC" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        ${toolchain:+-DCMAKE_TOOLCHAIN_FILE="$toolchain"} \
        -DBUILD_UI=OFF \
        -DDYLIB=OFF \
        "${extra[@]}" \
        > "$dir.log" 2>&1
    then
        echo "  CONFIGURE FAILED:"
        tail -25 "$dir.log" | sed 's/^/    /'
        failed+=("$name")
        return 0
    fi

    if ! cmake --build "$dir" -j "$JOBS" >> "$dir.log" 2>&1; then
        echo "  BUILD FAILED:"
        grep -E "error|FAILED" "$dir.log" | head -15 | sed 's/^/    /'
        failed+=("$name")
        return 0
    fi

    mkdir -p "$OUT"
    cp "$dir/disasterserver/$artifact" "$OUT/$outname"
    echo "  -> $outname"
    file -b "$OUT/$outname" | cut -c1-96 | sed 's/^/     /'
}

for t in "${targets[@]}"; do
    case "$t" in
        windows-x64)
            build_one "$t" "$SRC/docker/toolchain-mingw64.cmake" \
                DisasterServer.exe betterserver-windows-x64.exe ;;
        windows-x86)
            build_one "$t" "$SRC/docker/toolchain-mingw32.cmake" \
                DisasterServer.exe betterserver-windows-x86.exe ;;
        windows-arm64)
            build_one "$t" "$SRC/docker/toolchain-mingw-arm64.cmake" \
                DisasterServer.exe betterserver-windows-arm64.exe ;;
        windows-arm)
            build_one "$t" "$SRC/docker/toolchain-mingw-armv7.cmake" \
                DisasterServer.exe betterserver-windows-arm.exe ;;
        linux-x64)
            build_one "$t" "" \
                DisasterServer betterserver-linux-x64 \
                "-DCMAKE_EXE_LINKER_FLAGS=-static" ;;
        linux-x86)
            build_one "$t" "$SRC/docker/toolchain-i686-linux.cmake" \
                DisasterServer betterserver-linux-x86 ;;
        linux-arm64)
            build_one "$t" "$SRC/docker/toolchain-aarch64.cmake" \
                DisasterServer betterserver-linux-arm64 ;;
        linux-arm)
            build_one "$t" "$SRC/docker/toolchain-armhf.cmake" \
                DisasterServer betterserver-linux-arm ;;
        *)
            echo "Unknown target: $t" >&2
            echo "Valid: ${ALL_TARGETS[*]} all" >&2
            exit 2 ;;
    esac
done

echo ""
echo "=============================================================="
echo "  Done. Artifacts in dist/:"
echo "=============================================================="
ls -lh "$OUT" 2>/dev/null | tail -n +2 | awk '{printf "    %-38s %s\n", $9, $5}'

if [ ${#failed[@]} -gt 0 ]; then
    echo ""
    echo "  FAILED TARGETS: ${failed[*]}"
    exit 1
fi

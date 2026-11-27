#!/usr/bin/env bash
# ============================================================================
# build.sh — build the bridge on Linux/macOS with g++/clang++.
#
#   ./build.sh                        # uses g++, output build/bambu-bridge
#   ./build.sh --clang                # use clang++
#   ./build.sh --std=c++17 OUT=bin     # custom
# ============================================================================
set -euo pipefail

CXX="${CXX:-g++}"
STD="${STD:-c++17}"
OUT="${OUT:-build/bambu-bridge}"

# pick compiler
for a in "$@"; do
    case "$a" in
        --clang) CXX=clang++;;
    esac
done

codec_cflags=()
pkg-config --exists openh264 libjpeg || {
    echo '[build] Install the OpenH264 and libjpeg-turbo development packages (see README.md).' >&2
    exit 1
}
read -r -a codec_cflags <<< "$(pkg-config --cflags openh264 libjpeg)"
h264_lib="$(pkg-config --variable=libdir openh264)/libopenh264.a"
jpeg_lib="$(pkg-config --variable=libdir libjpeg)/libjpeg.a"
if [[ ! -f "$h264_lib" || ! -f "$jpeg_lib" ]]; then
    echo '[build] Static OpenH264 and libjpeg archives are required.' >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

echo "[build] $CXX -std=$STD -> $OUT"
"$CXX" -std="$STD" -O2 -Wall -Wextra \
    -Iinclude -Isrc "${codec_cflags[@]}" \
    src/main.cpp \
    src/bridge.cpp \
    src/plugin_loader.cpp \
    src/json.cpp \
    src/rpc_handler.cpp \
    src/stdio_transport.cpp \
    src/mcp_stdio.cpp \
    src/http_server.cpp \
    src/camera_stream.cpp \
    "$h264_lib" "$jpeg_lib" -pthread -ldl \
    -o "$OUT"

echo "[build] OK: $OUT"
echo "[build] example:"
echo "  $OUT --plugin-dir ./open_bambu_networking_plugin/bin/linux_x64 --country CN --http 8080 --stdio"

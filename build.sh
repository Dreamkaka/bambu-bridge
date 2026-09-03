#!/usr/bin/env bash
# ============================================================================
# build.sh — build the bridge on Linux/macOS with g++/clang++ (no CMake needed).
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

mkdir -p "$(dirname "$OUT")"

echo "[build] $CXX -std=$STD -> $OUT"
"$CXX" -std="$STD" -O2 -Wall -Wextra \
    -Iinclude -Isrc \
    -pthread -ldl \
    src/main.cpp \
    src/bridge.cpp \
    src/plugin_loader.cpp \
    src/json.cpp \
    src/rpc_handler.cpp \
    src/stdio_transport.cpp \
    src/http_server.cpp \
    src/camera_stream.cpp \
    -o "$OUT"

echo "[build] OK: $OUT"
echo "[build] example:"
echo "  $OUT --plugin-dir ./linux_02.05.02.58 --country CN --http 8080 --stdio"

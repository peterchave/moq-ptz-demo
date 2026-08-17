#!/bin/bash
# build.sh — Configure and build media_send.
#
# Usage:
#   ./build.sh              # build with defaults
#   ./build.sh clean        # wipe build/ and rebuild
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

if [[ "$1" == "clean" ]]; then
    rm -rf "$BUILD_DIR"
fi

cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR"
cmake --build "$BUILD_DIR" --target media_send

echo ""
echo "Built: $BUILD_DIR/media_send"
echo "Run:   $SCRIPT_DIR/rtsp_tx.sh"

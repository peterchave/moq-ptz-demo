#!/bin/bash
# build.sh — Configure and build ptz_control.
#
# Usage:
#   ./build.sh              # build with defaults
#   ./build.sh clean        # wipe build/ and rebuild
#
# The built binary lands at:
#   build/ptz_control
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

if [[ "$1" == "clean" ]]; then
    rm -rf "$BUILD_DIR"
fi

cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR"
cmake --build "$BUILD_DIR" --target ptz_control

echo ""
echo "Built: $BUILD_DIR/ptz_control"
echo "Run:   $SCRIPT_DIR/ptz_rx.sh"

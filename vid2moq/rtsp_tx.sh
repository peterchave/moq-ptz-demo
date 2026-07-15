#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ENV_FILE:-$SCRIPT_DIR/.env}"

# Load local secrets/config without committing them.
if [ -f "$ENV_FILE" ]; then
  set -a
  . "$ENV_FILE"
  set +a
fi

RTSP_URL="${RTSP_URL:?Set RTSP_URL in $ENV_FILE or the environment}"
RELAY="${RELAY:-moqt://moqrelay.example.com:443/moq-relay}"
NAMESPACE="${NAMESPACE:-namespace/live}"
TRACK="${TRACK:-video}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
FPS="${FPS:-30}"
BITRATE="${BITRATE:-4000000}"
CATALOG_KEEPALIVE_MS="${CATALOG_KEEPALIVE_MS:-10000}"

ffmpeg \
  -rtsp_transport tcp \
  -fflags nobuffer \
  -flags low_delay \
  -max_delay 0 \
  -reorder_queue_size 0 \
  -use_wallclock_as_timestamps 1 \
  -analyzeduration 100000 \
  -probesize 32768 \
  -i "$RTSP_URL" \
  -map 0:v:0 \
  -an \
  -c:v copy \
  -bsf:v h264_mp4toannexb \
  -f h264 pipe:1 | \
"$SCRIPT_DIR/build/media_send" \
  "$RELAY" \
  "$NAMESPACE" \
  "$TRACK" \
  --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" --bitrate "$BITRATE" \
  --catalog-keepalive-ms "$CATALOG_KEEPALIVE_MS" \
  --insecure-skip-verify
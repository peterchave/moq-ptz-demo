#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ENV_FILE:-$SCRIPT_DIR/.env}"

# Load local secrets/config without committing them.
if [ -f "$ENV_FILE" ]; then
  echo "Using environment file: $ENV_FILE"
  set -a
  . "$ENV_FILE"
  set +a
fi

echo "Configuration:"
echo "  RTSP_URL: $RTSP_URL"
echo "  RELAY: $RELAY"
echo "  NAMESPACE: $NAMESPACE"
echo "  TRACK: $TRACK"
echo "  WIDTH: $WIDTH"
echo "  HEIGHT: $HEIGHT"
echo "  FPS: $FPS"
echo "  BITRATE: $BITRATE"
echo "  CATALOG_KEEPALIVE_MS: $CATALOG_KEEPALIVE_MS"
echo ""

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


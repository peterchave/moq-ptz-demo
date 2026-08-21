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

IDLE_MS="${IDLE_MS:-5000}"

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
echo "  IDLE_MS: $IDLE_MS"
echo ""

# media_send announces the namespace immediately and pulls from the camera only
# while a subscriber is attached.
exec "$SCRIPT_DIR/build/media_send" \
  "$RELAY" \
  "$NAMESPACE" \
  --catalog-keepalive-ms "$CATALOG_KEEPALIVE_MS" \
  --insecure-skip-verify \
  --source cam --rtsp "$RTSP_URL" --idle-ms "$IDLE_MS" \
  --track "$TRACK" --from cam \
    --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" --bitrate "$BITRATE"
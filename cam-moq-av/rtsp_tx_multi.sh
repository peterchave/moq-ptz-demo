#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ENV_FILE:-$SCRIPT_DIR/.env}"

# Load local secrets/config without committing them.
if [ -f "$ENV_FILE" ]; then
  set -a
  . "$ENV_FILE"
  set +a
fi

TRACK_LOW="${TRACK_LOW:-${TRACK}-low}"
TRACK_KEYFRAMES="${TRACK_KEYFRAMES:-${TRACK}-keyframes}"
FPS_LOW="${FPS_LOW:-$FPS}"
RTSP_URL_LOW="${RTSP_URL_LOW:-${RTSP_URL//subtype=0/subtype=1}}"

# Hi-res is expensive: drop it soon after the last viewer leaves.
# Low-res is cheap and is what viewers subscribe to first, so let it linger.
IDLE_MS_HI="${IDLE_MS_HI:-5000}"
IDLE_MS_LOW="${IDLE_MS_LOW:-60000}"

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
echo "  RTSP_URL_LOW: $RTSP_URL_LOW"
echo "  TRACK_LOW: $TRACK_LOW"
echo "  WIDTH_LOW: $WIDTH_LOW"
echo "  HEIGHT_LOW: $HEIGHT_LOW"
echo "  BITRATE_LOW: $BITRATE_LOW"
echo "  TRACK_KEYFRAMES: $TRACK_KEYFRAMES"
echo "  IDLE_MS_HI / IDLE_MS_LOW: $IDLE_MS_HI / $IDLE_MS_LOW"
echo ""

# Two independent sources, each with its own ffmpeg child and lifecycle.
# The keyframe sidecar shares the hi-res source, so demand on it keeps hi-res up.
exec "$SCRIPT_DIR/build/media_send" \
  "$RELAY" \
  "$NAMESPACE" \
  --catalog-keepalive-ms "$CATALOG_KEEPALIVE_MS" \
  --insecure-skip-verify \
  --source hi  --rtsp "$RTSP_URL"     --idle-ms "$IDLE_MS_HI" \
  --source low --rtsp "$RTSP_URL_LOW" --idle-ms "$IDLE_MS_LOW" \
  --track "$TRACK" --from hi \
    --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" --bitrate "$BITRATE" \
  --track "$TRACK_LOW" --from low \
    --width "$WIDTH_LOW" --height "$HEIGHT_LOW" --fps "$FPS_LOW" --bitrate "$BITRATE_LOW" \
  --track "$TRACK_KEYFRAMES" --from hi --keyframes-only \
    --width "$WIDTH" --height "$HEIGHT" --bitrate "$BITRATE"


exit 0

exec "$SCRIPT_DIR/build/media_send" \
  "$RELAY" \
  "$NAMESPACE" \
  --catalog-keepalive-ms "$CATALOG_KEEPALIVE_MS" \
  --insecure-skip-verify \
  --source hi  --rtsp "$RTSP_URL"     --idle-ms "$IDLE_MS_HI" \
  --source low --rtsp "$RTSP_URL_LOW" --idle-ms "$IDLE_MS_LOW" \
  --track "$TRACK" --from hi \
    --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" --bitrate "$BITRATE" \
  --track "$TRACK_LOW" --from low \
    --width "$WIDTH_LOW" --height "$HEIGHT_LOW" --fps "$FPS_LOW" --bitrate "$BITRATE_LOW" \
  --track "$TRACK_KEYFRAMES" --from hi --keyframes-only \
    --width "$WIDTH" --height "$HEIGHT" --bitrate "$BITRATE"
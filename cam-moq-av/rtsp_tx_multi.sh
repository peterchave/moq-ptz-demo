#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ENV_FILE:-$SCRIPT_DIR/.env}"

# Load local secrets/config without committing them.
if [ -f "$ENV_FILE" ]; then
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
echo "  RTSP_URL_LOW: $RTSP_URL_LOW"
echo "  WIDTH_LOW: $WIDTH_LOW"
echo "  HEIGHT_LOW: $HEIGHT_LOW"
echo "  BITRATE_LOW: $BITRATE_LOW"
echo ""

# Named pipe for the low-res stream.
PIPE_LOW="$(mktemp -u /tmp/moq_low_XXXXXX)"
rm -f "$PIPE_LOW"   # remove any stale pipe from a previous run
mkfifo "$PIPE_LOW"

cleanup() {
    kill "$PID_LOW" 2>/dev/null
    rm -f "$PIPE_LOW"
}
trap cleanup EXIT INT TERM

# Low-res (subtype=1) → named pipe.
ffmpeg \
  -nostdin \
  -y \
  -rtsp_transport tcp \
  -fflags nobuffer \
  -flags low_delay \
  -max_delay 0 \
  -reorder_queue_size 0 \
  -use_wallclock_as_timestamps 1 \
  -analyzeduration 100000 \
  -probesize 32768 \
  -i "$RTSP_URL_LOW" \
  -map 0:v:0 \
  -an \
  -c:v copy \
  -bsf:v h264_mp4toannexb \
  -f h264 "$PIPE_LOW" &
PID_LOW=$!

# High-res (subtype=0) → stdin of media_send.
ffmpeg \
  -nostdin \
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
  --pipe "$TRACK_LOW" "$PIPE_LOW" "$WIDTH_LOW" "$HEIGHT_LOW" "$FPS_LOW" "$BITRATE_LOW" \
  --keyframe-track "$TRACK_KEYFRAMES" \
  --insecure-skip-verify
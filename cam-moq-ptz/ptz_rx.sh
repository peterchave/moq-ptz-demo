#!/bin/bash
# ptz_rx.sh — Start the PTZ control receiver.
#
# Connects to the MOQT relay, subscribes to the control namespace, and
# translates incoming atomic move commands into Amcrest HTTP API calls.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ENV_FILE:-$SCRIPT_DIR/.env}"

# Load local secrets/config without committing them.
if [ -f "$ENV_FILE" ]; then
  echo "Using environment file: $ENV_FILE"
  set -a
  . "$ENV_FILE"
  set +a
fi

BINARY="$SCRIPT_DIR/build/ptz_control"

exec "$BINARY" \
  "$RELAY" \
  "$NAMESPACE" \
  "$TRACK" \
  --cam-ip "$CAM_IP" \
  --cam-user "$CAM_USER" \
  --cam-pass "$CAM_PASS" \
  --cam-speed "$CAM_SPEED" \
  --max-duration-ms "$MAX_DURATION_MS" \
  --insecure-skip-verify

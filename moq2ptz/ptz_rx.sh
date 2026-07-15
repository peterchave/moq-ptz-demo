#!/bin/bash
# ptz_rx.sh — Start the PTZ control receiver.
#
# Connects to the MOQT relay, subscribes to the control namespace, and
# translates incoming atomic move commands into Amcrest HTTP API calls.
#
# The control namespace mirrors the live namespace with "/control" appended,
# matching the TypeScript publisher in moq-playa/examples/video/main.ts.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${ENV_FILE:-$SCRIPT_DIR/.env}"

# Load local secrets/config without committing them.
if [ -f "$ENV_FILE" ]; then
  set -a
  . "$ENV_FILE"
  set +a
fi

RELAY="${RELAY:-moqt://test2.moqx.akaleapi.net:443/moq-relay}"
NAMESPACE="${NAMESPACE:-ptz-cam-1/control}"
TRACK="${TRACK:-command}"

CAM_IP="${CAM_IP:?Set CAM_IP in $ENV_FILE or the environment}"
CAM_USER="${CAM_USER:?Set CAM_USER in $ENV_FILE or the environment}"
CAM_PASS="${CAM_PASS:?Set CAM_PASS in $ENV_FILE or the environment}"
CAM_SPEED="${CAM_SPEED:-5}"
MAX_DURATION_MS="${MAX_DURATION_MS:-2000}"

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

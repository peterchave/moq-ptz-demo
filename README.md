# moq-ptz-demo

PTZ demo split into three subprojects:

- [ui](ui/README.md): Browser UI for playback and PTZ controls.
- [vid2moq](vid2moq/README.md): RTSP camera video publisher to MOQT.
- [moq2ptz](moq2ptz/README.md): MOQT PTZ command receiver and camera control bridge.

## Prerequisites

### 1. moq5 + picoquic

Both `vid2moq` and `moq2ptz` link against prebuilt [moq5](https://github.com/openmoq/moq5) libraries
with its picoquic adapter. picoquic is **not bundled** in moq5 — clone it first, then build moq5
with the adapter flags enabled.

Complete these steps: [moq5 install](../MOQ5_INSTALL.md)

### 2. System packages

| Package | Required by |
|---------|------------|
| OpenSSL | moq2ptz, vid2moq |
| CMake ≥ 3.16 | moq2ptz, vid2moq |
| ffmpeg | vid2moq (runtime, piped) |
| curl | moq2ptz (runtime, camera CGI) |
| Node.js + pnpm | ui |

## Setup

```sh
# 1. Build C publishers/receivers
cd vid2moq  && ./build.sh && cd ..
cd moq2ptz  && ./build.sh && cd ..

# 2. Install UI dependencies
cd ui && pnpm install && cd ..
```

## Typical flow
1. Copy and configure `.env` in `moq2ptz` and `vid2moq` (`cp .env.example .env`).
2. Start the PTZ command receiver: `cd moq2ptz && ./ptz_rx.sh`
3. Start the video publisher: `cd vid2moq && ./rtsp_tx.sh`
4. Start the UI: `cd ui && pnpm dev`
5. Open the Vite URL in Chrome/Edge and connect to the same namespace.
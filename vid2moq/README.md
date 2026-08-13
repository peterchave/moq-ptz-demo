# vid2moq

Publishes a camera RTSP video stream into MOQT.

## What it does
- Reads H.264 video from an RTSP camera via ffmpeg.
- Pipes the stream into `media_send`.
- Publishes to a MOQT relay/namespace/track.

## Prerequisite: moq5 + picoquic

This project links against prebuilt libraries from [moq5](https://github.com/openmoq/moq5)
with its picoquic adapter. picoquic is **not bundled** in moq5 — clone it separately.

Complete these steps: [moq5 install](../MOQ5_INSTALL.md)

## Build
```sh
./build.sh
```

To rebuild from scratch:
```sh
./build.sh clean
```

## Configure
Create local secrets/config from the template:
```sh
cp .env.example .env
```
Config to your local environment

## Run
```sh
./rtsp_tx.sh
```

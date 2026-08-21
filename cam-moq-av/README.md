# vid2moq

Publishes a camera RTSP video stream into MOQT, on demand.

## What it does
- Announces the namespace and catalog to the relay at startup.
- Starts an `ffmpeg` child per *source* only once a track fed by that source
  gains a subscriber, and stops it again after the source's idle timeout.
- Publishes H.264 access units to one or more MOQT tracks.

A source is one RTSP input plus its ffmpeg process; a track is one MOQT track.
Several tracks may share a source (for example a keyframe-only sidecar), and
each source is supervised independently — one camera stream failing or
restarting never disturbs another.

## Configuration
Lifecycle tunables (see `.env.example`):
- `IDLE_MS` / `IDLE_MS_HI` / `IDLE_MS_LOW` — how long a source lingers after
  the last unsubscribe. Hysteresis matters: without it a page refresh causes an
  ffmpeg restart, and each restart costs an RTSP connect plus one GOP.

A source that dies unexpectedly is retried with exponential backoff (0.5s up to
30s) for as long as demand persists, so a flapping camera is not hammered.

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
Single stream:
```sh
./rtsp_tx.sh
```

Hi-res + low-res + keyframe sidecar, each on its own lifecycle:
```sh
./rtsp_tx_multi.sh
```

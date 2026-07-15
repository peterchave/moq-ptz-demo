# vid2moq

Publishes a camera RTSP video stream into MOQT.

## What it does
- Reads H.264 video from an RTSP camera via ffmpeg.
- Pipes the stream into `media_send`.
- Publishes to a MOQT relay/namespace/track.

## Prerequisite: local moq5 checkout
This project links against prebuilt libraries from a local `moq5` repository.

If you do not already have one:
```sh
git clone https://github.com/openmoq/moq5
```

By default, this project expects moq5 at `/Users/pchave/Documents/Alpha/MoQ/ptz-demo/moq5`.
If your checkout is elsewhere, pass `-DMOQ5_ROOT=/path/to/moq5` when running cmake.

## Build
```sh
./build.sh
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

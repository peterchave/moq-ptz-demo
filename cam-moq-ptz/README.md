# moq2ptz

Consumes PTZ commands from MOQT and sends camera movement commands over HTTP.

## What it does
- Subscribes to a MOQT control namespace/track.
- Receives PTZ command messages.
- Translates commands to Amcrest-compatible camera API calls.
- Automatically retries if the publisher is not yet running.

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
Configure for your local environment

## Run
```sh
./ptz_rx.sh
```

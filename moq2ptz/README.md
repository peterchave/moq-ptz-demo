# moq2ptz

Consumes PTZ commands from MOQT and sends camera movement commands over HTTP.

## What it does
- Subscribes to a MOQT control namespace/track.
- Receives PTZ command messages.
- Translates commands to Amcrest-compatible camera API calls.

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
Configure for your local environment

## Run
```sh
./ptz_rx.sh
```

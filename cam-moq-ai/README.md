# moq2ai

Streams camera frames into MOQT, runs YOLO detection, and publishes detection results for overlaying on the UI.

## What it does
- Subscribes to a live camera stream from the relay.
- Decodes frames and converts them to JPEG.
- Sends each frame to a YOLO inference service.
- Publishes detection JSON to a MOQT object track for the browser overlay.

## Build
```sh
npm install
```

## Configure
Create local secrets/config from the template:
```sh
cp .env.example .env
```
Configure the relay URL, namespace, and YOLO endpoint for your environment.

## Run
```sh
./moq_ai.sh
```


## Exmaple YOLO service
[exmaple-yolo](exmaple-yolo/README.md)
# Simple YOLO example service

This folder contains a minimal FastAPI server that runs Ultralytics YOLO on uploaded images and exposes a `/predict` endpoint for the MOQT AI overlay.

## What it does
- Loads a YOLO model on startup.
- Accepts an uploaded image at `POST /predict`.
- Returns bounding boxes, labels, and confidence values in JSON.

## Install
From this folder:

```sh
./install.sh
```

This creates a Python virtual environment, installs the required packages, and installs a systemd service named `yolo-api`.

## Run manually
If you want to launch it without systemd:

```sh
cd /Users/pchave/Documents/Alpha/MoQ/ptz-demo-2/cam-moq-ai/example-yolo
python3 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install ultralytics fastapi uvicorn python-multipart pillow
uvicorn main:app --host 0.0.0.0 --port 8000
```

## Check it is running
Open:

```sh
http://localhost:8000/
```

You should see a health response from the app.

## API usage
Upload an image to:

```sh
http://localhost:8000/predict
```

The API expects a multipart form upload with a file field named `file`.

## Configure the MOQT AI client
Point the AI client at the service URL used here, for example:

```env
YOLO_URL=http://localhost:8000/predict
```

import io
from fastapi import FastAPI, UploadFile, File, HTTPException
from PIL import Image
from ultralytics import YOLO

app = FastAPI(title="YOLO Object Detection API")

# Load YOLO model on startup (yolov11l.pt will auto-download on first execution)
model = YOLO("yolov11l.pt")

@app.get("/")
def health_check():
    return {"status": "ok", "message": "YOLO API is operational"}

@app.post("/predict")
async def predict(file: UploadFile = File(...), conf: float = 0.25):
    if not file.content_type.startswith("image/"):
        raise HTTPException(status_code=400, detail="File uploaded must be an image.")
    
    try:
        # Load binary image into PIL
        image_bytes = await file.read()
        image = Image.open(io.BytesIO(image_bytes)).convert("RGB")
        
        # Run YOLO inference
        results = model(image, conf=conf)[0]
        
        # Extract object detection bounding boxes
        detections = []
        for box in results.boxes:
            cls_id = int(box.cls[0])
            label = results.names[cls_id]
            confidence = float(box.conf[0])
            xyxy = box.xyxy[0].tolist()  # [xmin, ymin, xmax, ymax]
            
            detections.append({
                "class_id": cls_id,
                "label": label,
                "confidence": round(confidence, 4),
                "bbox": [round(coord, 2) for coord in xyxy]
            })
            
        return {
            "success": True,
            "count": len(detections),
            "predictions": detections
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
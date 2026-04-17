from flask import Flask, request, jsonify
from ultralytics import YOLO
from PIL import Image
import io

app = Flask(__name__)
model = YOLO("yolov8n.pt")  # download otomatis ~6MB, model nano tercepat

@app.route("/detect", methods=["POST"])
def detect():
    if "image" not in request.files:
        return jsonify({"error": "No image"}), 400

    img_bytes = request.files["image"].read()
    img = Image.open(io.BytesIO(img_bytes)).convert("RGB")

    results = model(img, conf=0.4)[0]  # confidence threshold 40%

    detections = []
    for box in results.boxes:
        detections.append({
            "label": model.names[int(box.cls)],
            "confidence": round(float(box.conf), 2),
            "box": [round(float(x), 1) for x in box.xyxy[0].tolist()]
        })

    return jsonify({
        "count": len(detections),
        "detections": detections
    })

if __name__ == "__main__":
    # Ganti 0.0.0.0 agar bisa diakses dari ESP32 di LAN
    app.run(host="0.0.0.0", port=5000, debug=False)
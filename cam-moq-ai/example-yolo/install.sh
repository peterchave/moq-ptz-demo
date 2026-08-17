# Install dependencies
apt update && apt upgrade -y
apt install -y python3-pip python3-venv libgl1 libglib2.0-0 ffmpeg ufw

# Create a virtual environment and install required packages
mkdir ~/yolo-api && cd ~/yolo-api
python3 -m venv venv
source venv/bin/activate

# Install required Python packages
pip install --upgrade pip
pip install ultralytics fastapi uvicorn python-multipart pillow

# Create a systemd service file for the YOLO API
cat <<EOL > /etc/systemd/system/yolo-api.service
[Unit]
Description=YOLO FastAPI Server
After=network.target

[Service]
User=root
WorkingDirectory=/root/yolo-api
ExecStart=/root/yolo-api/venv/bin/uvicorn main:app --host 0.0.0.0 --port 8000
Restart=always

[Install]
WantedBy=multi-user.target
EOL

# Enable and start the YOLO API service
systemctl daemon-reload
systemctl enable yolo-api
systemctl start yolo-api
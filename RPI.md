# Installing MOQ bridge on Raspberry Pi

MOQ bridge (cam-moq-av + cam-moq-ptz)

Tested with Raspberry Pi model 3

### Steps

Starting with Raspberry Pi OS Lite (Debain Tixie) 

Install requirements:
```apt update && apt install git cmake lib-ssl ffmpeg```

Clone repo
Follow MOQ5_INSTALL.md
Compile cam-moq-av and cam-moq-ptz locally
Setup service to run them.
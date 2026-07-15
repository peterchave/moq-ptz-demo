# moq-ptz-demo

PTZ demo split into three subprojects:

- [ui](ui/README.md): Browser UI for playback and PTZ controls.
- [vid2moq](vid2moq/README.md): RTSP camera video publisher to MOQT.
- [moq2ptz](moq2ptz/README.md): MOQT PTZ command receiver and camera control bridge.

## Typical flow
1. Start the PTZ command receiver in `moq2ptz`.
2. Start the video publisher in `vid2moq`.
3. Run the web app in `ui` and connect to the same namespace.
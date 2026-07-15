# PTZ Demo


## User interface

Built from `examples/video` demo from `moq-playa`
Enables playback + remote PTZ control over a MOQT connection

### Run locally

```sh
npm install
npm run dev
```
Open the local Vite URL in Chrome/Edge.

### Query Params
- `url`: Relay URL (default: `<origin>:4433`)
- `ns`: Namespace root (default: `live`)
- `hash`: Relay certificate SHA-256 hash (hex)

Example:
```text
http://localhost:5173/index.html?url=https://moq.relay.example/moq-relay&ns=ptz-cam-1
```

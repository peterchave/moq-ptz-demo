# PTZ Demo UI

## What it does

Browser-based playback and remote PTZ control over a MOQT connection.
Built from the `examples/video` demo in `moq-playa`.

## Install

```sh
pnpm install
```

## Run

```sh
pnpm dev
```

Open the local Vite URL in Chrome or Edge.

## Query params

| Param | Default | Description |
|-------|---------|-------------|
| `url` | `<origin>:4433` | Relay URL |
| `ns`  | `live` | Namespace root |
| `hash` | — | Relay certificate SHA-256 hash (hex) |

Example:
```
http://localhost:5173/?url=https://moq.relay.example/moq-relay&ns=ptz-cam-1
```

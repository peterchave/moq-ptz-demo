# trixie (glibc 2.41): the prebuilt quiche addon requires glibc >= 2.38.
FROM node:22-trixie-slim

WORKDIR /app

# Install runtime dependencies used by the MoQ client and frame conversion path.
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    ca-certificates \
    ffmpeg \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Expect build context at repository root:
# docker build -f cam-moq-ai/Dockerfile -t cam-moq-ai .
# Deps live at /app so the shared @moqt sources under /app/cam-moq-ui also resolve them.
COPY cam-moq-ai/package*.json ./
RUN npm install

COPY cam-moq-ai/ ./cam-moq-ai/
# main.ts resolves @moqt/* via tsconfig paths pointing at ../cam-moq-ui/packages.
COPY cam-moq-ui/packages/ ./cam-moq-ui/packages/

WORKDIR /app/cam-moq-ai

# Default: use local .env if present in the container or pass environment variables at runtime.
ENV NODE_ENV=production

CMD ["bash", "-lc", "if [ -f .env ]; then set -a && . ./.env && set +a; fi; npm start"]

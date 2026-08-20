FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG MOQ5_REPO=https://github.com/openmoq/moq5.git
ARG PICOQUIC_REPO=https://github.com/openmoq/picoquic.git
ARG PICOTLS_REPO=https://github.com/h2o/picotls.git

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    ca-certificates \
    cmake \
    curl \
    git \
    build-essential \
    pkg-config \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Build moq5 + picoquic + picotls stack required by CMakeLists imports.
RUN git clone --depth=1 "$MOQ5_REPO" moq5 && \
    git clone --depth=1 "$PICOQUIC_REPO" moq5/thirdparty/picoquic && \
    git clone --depth=1 "$PICOTLS_REPO" moq5/thirdparty/picotls && \
    git -C moq5/thirdparty/picotls submodule update --init --recursive

RUN cmake -B /app/moq5/thirdparty/picotls/build -S /app/moq5/thirdparty/picotls \
      -DWITH_OPENSSL=ON -DBUILD_TESTING=OFF && \
    cmake --build /app/moq5/thirdparty/picotls/build

RUN cmake -B /app/moq5/build -S /app/moq5 \
      -DMOQ_BUILD_ADAPTER_PICOQUIC=ON \
      -DMOQ_BUILD_PQ_THREADED=ON \
      -DMOQ_BUILD_SERVICE=ON \
      -DMOQ_BUILD_MSF=ON \
      -DMOQ_PICOQUIC_SOURCE_DIR=/app/moq5/thirdparty/picoquic && \
    cmake --build /app/moq5/build

# Expect build context at repository root:
# docker build -f cam-moq-ptz/Dockerfile -t cam-moq-ptz .
COPY cam-moq-ptz /app/cam-moq-ptz

RUN cmake -B /app/cam-moq-ptz/build -S /app/cam-moq-ptz -DMOQ5_ROOT=/app/moq5 && \
    cmake --build /app/cam-moq-ptz/build --target ptz_control


FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    ca-certificates \
    curl \
    openssl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app/cam-moq-ptz

COPY --from=builder /app/cam-moq-ptz/ptz_rx.sh /app/cam-moq-ptz/ptz_rx.sh
COPY --from=builder /app/cam-moq-ptz/.env.example /app/cam-moq-ptz/.env.example
COPY --from=builder /app/cam-moq-ptz/build/ptz_control /app/cam-moq-ptz/build/ptz_control

RUN chmod +x /app/cam-moq-ptz/ptz_rx.sh /app/cam-moq-ptz/build/ptz_control

ENTRYPOINT ["/app/cam-moq-ptz/ptz_rx.sh"]
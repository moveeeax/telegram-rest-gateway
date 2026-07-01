# syntax=docker/dockerfile:1.7
# Основной multi-stage образ сервиса. TDLib берётся из кэш-образа tdlib-base:${TDLIB_REF}
# (см. Dockerfile.tdlib), Drogon собирается статически, финал — distroless nonroot.
# Multi-arch (amd64+arm64) собирается CI нативно на per-arch раннерах + manifest merge.
ARG TDLIB_REF=master
ARG DROGON_REF=v1.9.1

FROM tdlib-base:${TDLIB_REF} AS tdlib

# ---------- builder ----------
FROM debian:12-slim AS builder
ARG DROGON_REF
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates cmake ninja-build g++-12 \
        zlib1g-dev libssl-dev libjsoncpp-dev uuid-dev \
        libc-ares-dev libbrotli-dev \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-12 CXX=g++-12

# Drogon статически (§11.10): без ORM/YAML/Brotli-обвязки — минимум зависимостей.
WORKDIR /drogon
RUN git clone --recursive https://github.com/drogonframework/drogon.git . \
    && git checkout "${DROGON_REF}" && git submodule update --init --recursive \
    && cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/drogon \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_ORM=OFF -DBUILD_YAML_CONFIG=OFF -DBUILD_EXAMPLES=OFF \
    && cmake --build build --target install

# TDLib из кэш-образа.
COPY --from=tdlib /opt/tdlib /opt/tdlib

# Наш сервис.
WORKDIR /app
COPY . .
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DTGW_BUILD_TESTS=OFF \
        -DCMAKE_PREFIX_PATH="/opt/tdlib;/opt/drogon" \
    && cmake --build build --target telegram-rest-gateway

# Каталоги данных с нужными правами/владельцем (distroless не умеет chown в рантайме).
RUN install -d -m 0700 -o 65532 -g 65532 /data/session /data/files

# ---------- runtime (distroless nonroot) ----------
FROM gcr.io/distroless/cc-debian12:nonroot AS runtime
# TDLib слинкована статически; из динамических нужны только OpenSSL/ZLib.
COPY --from=builder /usr/lib/*/libssl.so.3    /usr/lib/
COPY --from=builder /usr/lib/*/libcrypto.so.3 /usr/lib/
COPY --from=builder /usr/lib/*/libz.so.1      /usr/lib/
COPY --from=builder /app/build/telegram-rest-gateway /app/telegram-rest-gateway
COPY --from=builder --chown=65532:65532 /data /data

USER 65532:65532
VOLUME ["/data/session", "/data/files"]
ENV TGW_LISTEN_ADDRESS=0.0.0.0 \
    TGW_LISTEN_PORT=8080
EXPOSE 8080

# distroless без shell/curl → healthcheck подкомандой бинаря (§11.8).
HEALTHCHECK --interval=15s --timeout=3s --start-period=20s \
    CMD ["/app/telegram-rest-gateway", "--healthcheck"]

ENTRYPOINT ["/app/telegram-rest-gateway"]

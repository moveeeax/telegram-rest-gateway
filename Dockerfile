# syntax=docker/dockerfile:1.7
# Образ сервиса. FROM builder (см. Dockerfile.builder) — TDLib/Drogon уже собраны,
# пересобирается только наш код. Финал — distroless nonroot. Multi-arch собирает CI
# нативно на per-arch раннерах (решение C9) + manifest merge.
ARG BUILDER_IMAGE
FROM ${BUILDER_IMAGE} AS builder

WORKDIR /app
COPY . .
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DTGW_BUILD_TESTS=OFF \
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

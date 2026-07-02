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

# Собираем ВСЕ динамические зависимости бинаря по ldd (OpenSSL/ZLib/c-ares/brotli/jsoncpp/uuid…),
# кроме glibc/gcc-ядра (libc/libm/libdl/libpthread/librt/libstdc++/libgcc_s/ld-linux) — оно уже
# есть в distroless/cc. TDLib слинкована статически, поэтому её .so тут нет.
RUN mkdir -p /dist-libs && \
    ldd /app/build/telegram-rest-gateway \
      | awk '/=> \//{print $3}' \
      | grep -vE '/(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|ld-linux[^/]*)\.so' \
      | xargs -I{} cp -Lv {} /dist-libs/

# Каталоги данных с нужными правами/владельцем (distroless не умеет chown в рантайме).
RUN install -d -m 0700 -o 65532 -g 65532 /data/session /data/files

# ---------- runtime (distroless nonroot) ----------
FROM gcr.io/distroless/cc-debian12:nonroot AS runtime
COPY --from=builder /dist-libs/ /usr/lib/
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

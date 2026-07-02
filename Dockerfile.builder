# syntax=docker/dockerfile:1.7
# Builder-образ: тулчейн + СТАТИЧЕСКИЕ TDLib(@TDLIB_REF) и Drogon(@DROGON_REF) в /opt.
# Дорогой (сборка TDLib долгая); собирается ВРУЧНУЮ per-arch при смене пинов.
# Используется: test/tidy-job'ами (image: <registry>:builder-<arch>) и основным Dockerfile
# (FROM ...:builder-<arch>) — сервис пересобирается быстро, TDLib/Drogon не трогаются.
# Build: docker build -f Dockerfile.builder --build-arg TDLIB_REF=<sha> --build-arg DROGON_REF=<tag> .
ARG TDLIB_REF=master
ARG DROGON_REF=v1.9.1

FROM debian:12-slim
ARG TDLIB_REF
ARG DROGON_REF
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates cmake ninja-build g++-12 clang-format clang-tidy \
        gperf zlib1g-dev libssl-dev libjsoncpp-dev uuid-dev libc-ares-dev libbrotli-dev \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-12 CXX=g++-12

# --- TDLib (static, Td::TdStatic). Нативно per-arch; >=8 ГБ RAM на td_api.cpp. ---
RUN git clone https://github.com/tdlib/td.git /src/td \
    && git -C /src/td checkout "${TDLIB_REF}" \
    && cmake -S /src/td -B /src/td/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/tdlib \
        -DTD_ENABLE_LTO=OFF -DTD_ENABLE_JNI=OFF \
    && cmake --build /src/td/build --target install \
    && rm -rf /src/td

# --- Drogon (static). ---
RUN git clone --recursive https://github.com/drogonframework/drogon.git /src/drogon \
    && git -C /src/drogon checkout "${DROGON_REF}" \
    && git -C /src/drogon submodule update --init --recursive \
    && cmake -S /src/drogon -B /src/drogon/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/drogon \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_ORM=OFF -DBUILD_YAML_CONFIG=OFF -DBUILD_EXAMPLES=OFF \
    && cmake --build /src/drogon/build --target install \
    && rm -rf /src/drogon

ENV CMAKE_PREFIX_PATH=/opt/tdlib:/opt/drogon

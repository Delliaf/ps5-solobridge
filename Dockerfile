# Ambiente di build per payload PS5 (ps5-payload-dev/sdk)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        bash clang-18 lld-18 llvm-18 wget unzip make ca-certificates \
        socat cmake meson pkg-config python3 python3-pyelftools netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

ENV LLVM_CONFIG=/usr/bin/llvm-config-18
RUN wget -q https://github.com/ps5-payload-dev/sdk/releases/download/v0.50/ps5-payload-sdk.zip -O /tmp/sdk.zip \
    && echo '2993a69e52fea04a32bf1753318fc60b3f918ab635bdf946870e7d1849557bb2  /tmp/sdk.zip' | sha256sum -c - \
    && unzip -q -d /opt /tmp/sdk.zip \
    && rm /tmp/sdk.zip

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORKDIR /work

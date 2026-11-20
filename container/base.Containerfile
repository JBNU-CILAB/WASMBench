# Shared toolchain for every workload image: wasi-sdk for the wasm build, gcc
# for the native reference build, python3 for build.py.
ARG UBUNTU=docker.io/library/ubuntu:24.04@sha256:77eb101c607d6e3d4f5ad0c8a02c2136c689b98aba1f359e6502ba517e5c57ec

FROM ${UBUNTU} AS fetch
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates curl
# wasi-sdk 34, checked against the digests GitHub publishes for the release.
ARG TARGETARCH
RUN set -eu; \
    case "${TARGETARCH:-$(dpkg --print-architecture)}" in \
      amd64) a=x86_64; sum=b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4 ;; \
      arm64) a=arm64;  sum=f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971 ;; \
      *) echo "unsupported architecture" >&2; exit 1 ;; \
    esac; \
    f=wasi-sdk-34.0-$a-linux.tar.gz; \
    curl -fsSL -o /tmp/$f https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/$f; \
    echo "$sum  /tmp/$f" | sha256sum -c -; \
    mkdir /opt/wasi-sdk; \
    tar -xzf /tmp/$f -C /opt/wasi-sdk --strip-components=1

FROM ${UBUNTU}
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc libc6-dev python3 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=fetch /opt/wasi-sdk /opt/wasi-sdk
ENV WASI_SDK=/opt/wasi-sdk \
    HOST_CC=gcc \
    OUT=/out \
    BLD=/build
WORKDIR /src

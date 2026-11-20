# FFmpeg libavcodec + x264: encode or decode a small random-noise clip.
# libavcodec and x264 are built (wasm + native) in their own layer, keyed on
# the compile options only, so changing defines or ldflags rebuilds just the driver.
ARG BASE=wasmbench-base:latest
FROM ${BASE}
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends make pkg-config \
 && rm -rf /var/lib/apt/lists/*
COPY build.py ./
COPY third_party/ffmpeg third_party/ffmpeg
COPY third_party/x264 third_party/x264
COPY workloads/media workloads/media
ARG WASMBENCH_OPT=-O2
ARG WASMBENCH_CFLAGS=[]
RUN python3 build.py --inside ffmpeg --prepare
COPY workloads/bench.h workloads/corpus.h workloads/ffmpeg.c workloads/
ARG WASMBENCH_LDFLAGS=[]
ARG WASMBENCH_DEFS=[]
RUN python3 build.py --inside ffmpeg

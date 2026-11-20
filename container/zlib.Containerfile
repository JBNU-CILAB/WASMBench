# madler/zlib: deflate + inflate roundtrip.
ARG BASE=wasmbench-base:latest
FROM ${BASE}
COPY build.py ./
COPY third_party/zlib third_party/zlib
COPY workloads/bench.h workloads/corpus.h workloads/zlib.c workloads/
ARG WASMBENCH_OPT=-O2
ARG WASMBENCH_CFLAGS=[]
ARG WASMBENCH_LDFLAGS=[]
ARG WASMBENCH_DEFS=[]
RUN python3 build.py --inside zlib

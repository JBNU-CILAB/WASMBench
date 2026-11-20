# ezXML: parse and walk a generated document.
ARG BASE=wasmbench-base:latest
FROM ${BASE}
COPY build.py ./
COPY third_party/ezxml third_party/ezxml
COPY workloads/bench.h workloads/corpus.h workloads/xml.c workloads/
ARG WASMBENCH_OPT=-O2
ARG WASMBENCH_CFLAGS=[]
ARG WASMBENCH_LDFLAGS=[]
ARG WASMBENCH_DEFS=[]
RUN python3 build.py --inside xml

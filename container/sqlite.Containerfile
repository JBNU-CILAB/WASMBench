# SQLite: queries on an in-memory database. sqlite3.c is generated from the
# source tree (configure + make + tclsh) in its own layer, so changing the
# compile options or defines does not regenerate it.
ARG BASE=wasmbench-base:latest
FROM ${BASE}
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends make tcl \
 && rm -rf /var/lib/apt/lists/*
COPY build.py ./
COPY third_party/sqlite third_party/sqlite
RUN python3 build.py --inside sqlite --prepare
COPY workloads/bench.h workloads/corpus.h workloads/sqlite.c workloads/
ARG WASMBENCH_OPT=-O2
ARG WASMBENCH_CFLAGS=[]
ARG WASMBENCH_LDFLAGS=[]
ARG WASMBENCH_DEFS=[]
RUN python3 build.py --inside sqlite

# Lua 5.5 running a word-count / pattern-matching script.
ARG BASE=wasmbench-base:latest
FROM ${BASE}
COPY build.py ./
COPY third_party/lua third_party/lua
COPY workloads/bench.h workloads/corpus.h workloads/lua.c workloads/setjmp.h workloads/
ARG WASMBENCH_OPT=-O2
ARG WASMBENCH_CFLAGS=[]
ARG WASMBENCH_LDFLAGS=[]
ARG WASMBENCH_DEFS=[]
RUN python3 build.py --inside lua

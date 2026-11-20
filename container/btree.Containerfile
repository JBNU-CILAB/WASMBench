# tidwall/btree.c: insert, lookup, ordered scan, delete.
ARG BASE=wasmbench-base:latest
FROM ${BASE}
COPY build.py ./
COPY third_party/btree third_party/btree
COPY workloads/bench.h workloads/corpus.h workloads/btree.c workloads/
ARG WASMBENCH_OPT=-O2
ARG WASMBENCH_CFLAGS=[]
ARG WASMBENCH_LDFLAGS=[]
ARG WASMBENCH_DEFS=[]
RUN python3 build.py --inside btree

# WASMBench

Real workload based WebAssembly engine performance benchmark suite.

## Workloads

| workload | description |
|---|---|
| `sqlite` | SQLite 3.45.1, queries on an in-memory database |
| `json`   | cJSON, parse and walk a generated document |
| `btree`  | tidwall/btree.c, insert / lookup / scan / delete |
| `xml`    | ezXML 0.8.6, parse and walk a generated document |
| `zlib`   | madler/zlib, deflate + inflate roundtrip |
| `lua`    | Lua 5.5, word-count and pattern-matching script |

## Build

```sh
git submodule update --init --depth 1
./build.sh
./build.sh sqlite zlib
DEFS_sqlite="-DROWS=5000" ./build.sh sqlite
```

Requires wasi-sdk (`WASI_SDK`), a host C compiler and `tclsh`.
Each module runs without arguments and prints one number, which must equal the
value in `artifacts/expected.json`.

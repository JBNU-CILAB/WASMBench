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
| `ffmpeg` | FFmpeg 8.1.3 libavcodec + x264, encode or decode a 2-second random-noise clip |

## Build

```sh
git submodule update --init --depth 1
./build.sh
./build.sh sqlite zlib
DEFS_sqlite="-DROWS=5000" ./build.sh sqlite
DEFS_ffmpeg="-DENCODER=mpeg4 -DMODE=1 -DITERS=4" ./build.sh ffmpeg
```

Requires wasi-sdk (`WASI_SDK`), a host C compiler and `tclsh`, plus `make` and
`pkg-config` for `ffmpeg`. The `ffmpeg` encoder is one of `libx264` (default),
`mpeg4`, `mpeg2video`, `mjpeg`, `prores_ks` and `ffv1`; `MODE=0` decodes and
`MODE=1` encodes; `ITERS` (default 64 for decode, 8 for encode) sets the
measured iterations. Its libraries are built once per target under `build/` and
`ffmpeg` is not available with `TOOLCHAIN=emscripten`. The source clip is
embedded in the module; `workloads/media/README.md` shows how it was made.
Each module runs without arguments and prints one number, which must equal the
value in `artifacts/expected.json`.

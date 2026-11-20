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
./build.py                  # workloads enabled in config.json
./build.py sqlite zlib
```

Requires podman or docker. Settings are in `config.json`: compile options and
each workload's `-D` knobs (documented at the top of `workloads/<name>.c`).

```json
"workloads": { "ffmpeg": { "enabled": true, "defines": { "ENCODER": "mpeg4", "MODE": 1 } } }
```

Each module runs without arguments and prints one number, which must equal the
value in `artifacts/expected.json`. The `ffmpeg` source clip is embedded in the
module; `workloads/media/README.md` shows how it was made.

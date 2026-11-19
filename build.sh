#!/usr/bin/env bash
# Build the WASMBench workloads into standalone wasm32-wasip1 modules, in the
# shape of walrus' test/wasmBenchmarker ctests: each module takes no arguments,
# runs `runtime()` and prints one number.
#
#   ./build.sh                          build every workload with wasi-sdk
#   ./build.sh sqlite zlib              build only the named ones
#   DEFS_sqlite="-DROWS=5000 -DPREPARED=1" ./build.sh sqlite
#   DEFS="-DSEED=CORPUS_SEED_TRAIN" ./build.sh    -D for every workload
#   OPT=-O3 ./build.sh                  optimisation level (default -O2)
#   TOOLCHAIN=emscripten ./build.sh     emcc -sSTANDALONE_WASM instead of wasi-sdk
#
# Workloads — libraries are git submodules under third_party/
# (`git submodule update --init --depth 1` first); knobs are documented at the
# top of each workloads/<name>.c:
#
#   sqlite   SQLite 3.45.1 (amalgamation generated from the source tree)
#   json     cJSON parse + tree walk
#   btree    tidwall/btree.c: insert, lookup, ordered scan, delete
#   xml      ezXML 0.8.6 parse + tree walk
#   zlib     madler/zlib deflate / inflate
#   lua      Lua 5.5 interpreter running a word-count / pattern-match script
#   ffmpeg   FFmpeg 8.1.3 libavcodec + x264: encode / decode a small random-noise clip
#            (DEFS_ffmpeg=-DENCODER=mpeg4 picks the encoder; wasi-sdk only)
#
# Expected values: every workload is also compiled natively (HOST_CC) from the
# same source with the same -D flags and run once; its output is the expected
# value, written to artifacts/expected.json and used by benchmark.py. A tuned
# build therefore gets checked as strictly as the default one, against a
# reference that did not come from the engine under test.
#
# Output: artifacts/<name>.wasm, artifacts/expected.json, artifacts/MANIFEST.tsv
set -euo pipefail

W=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
TP=$W/third_party
WL=$W/workloads
OUT=${OUT:-$W/artifacts}
BLD=$W/build
mkdir -p "$OUT" "$BLD/native"

ALL="sqlite json btree xml zlib lua ffmpeg"
WORKLOADS=${*:-${WORKLOADS:-$ALL}}
TOOLCHAIN=${TOOLCHAIN:-wasi-sdk}
OPT=${OPT:--O2}
DEFS=${DEFS:-}
HOST_CC=${HOST_CC:-cc}
VERIFY=${VERIFY:-1}
WALRUS=${WALRUS:-$W/../walrus/out/walrus}

case $TOOLCHAIN in
wasi-sdk)
  WASI_SDK=${WASI_SDK:-$HOME/opt/wasi-sdk-34.0-x86_64-linux}
  CC=${CC:-$WASI_SDK/bin/clang}
  [ -x "$CC" ] || { echo "no wasi-sdk clang at $CC (set WASI_SDK or CC)" >&2; exit 1; }
  # wasi-sdk's clang.cfg already selects --target=wasm32-wasip1 and the sysroot.
  TLDFLAGS=""
  LUA_SIGNAL_LIB="-lwasi-emulated-signal"
  ;;
emscripten)
  CC=${CC:-emcc}
  command -v "$CC" >/dev/null || { echo "emcc not found (source emsdk_env.sh)" >&2; exit 1; }
  # STANDALONE_WASM emits a plain module with wasi_snapshot_preview1 imports and
  # a _start export instead of a JS-glue module; walrus runs it like wasi-sdk's.
  TLDFLAGS="-sSTANDALONE_WASM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB -sINITIAL_MEMORY=32MB"
  LUA_SIGNAL_LIB=""
  ;;
*) echo "unknown TOOLCHAIN=$TOOLCHAIN (wasi-sdk | emscripten)" >&2; exit 1 ;;
esac

# SQLite: drop everything that is impossible on wasip1 or that would measure
# something other than the query engine. The driver opens ":memory:".
SQLFLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL"
SQLFLAGS="$SQLFLAGS -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_MEMSTATUS=0"

# Lua freestanding on wasm without the exception-handling proposal:
#   luai_makeseed()=0u   fixed string-hash seed (otherwise stack address + clock)
#   LUAI_THROW/LUAI_TRY  no setjmp/longjmp; a script error aborts loudly
#   workloads/setjmp.h   shadows the sysroot's #error header
#   L_tmpnam + stubs     loslib's hosted-C references (see workloads/lua.c)
LUAFLAGS="-DMAKE_LIB -Dluai_makeseed()=0u -DL_tmpnam=32 -D_WASI_EMULATED_SIGNAL"
LUAFLAGS="$LUAFLAGS -Dluai_jmpbuf=int -DLUAI_THROW(L,c)=abort() -DLUAI_TRY(L,c,f,ud)=((f)(L,ud))"

ZLIB_SRCS="adler32 crc32 deflate inflate inftrees inffast trees zutil compress uncompr"

# --- third_party submodules --------------------------------------------------
declare -A SUBMOD=( [sqlite]=sqlite [json]=cjson [btree]=btree [xml]=ezxml [zlib]=zlib [lua]=lua
                    [ffmpeg]="ffmpeg x264" )
for w in $WORKLOADS; do
  dirs=${SUBMOD[$w]:-}
  [ -n "$dirs" ] || { echo "unknown workload: $w (known: $ALL)" >&2; exit 1; }
  for d in $dirs; do
    [ -e "$TP/$d/.git" ] || {
      echo "third_party/$d is empty — run: git submodule update --init --depth 1" >&2; exit 1; }
  done
done
if [[ " $WORKLOADS " == *" ffmpeg "* && $TOOLCHAIN != wasi-sdk ]]; then
  echo "ffmpeg is built with wasi-sdk only" >&2
  exit 1
fi

# --- SQLite amalgamation -----------------------------------------------------
# The submodule is the canonical sqlite/sqlite source tree; sqlite3.c is
# generated from it (host cc + tclsh) into build/sqlite and regenerated when the
# submodule moves. Generated from tag version-3.45.1 it is identical to the
# sqlite.org release amalgamation except for the 4-line provenance comment.
SQLITE_BLD=$BLD/sqlite
sqlite_amalgamation() {
  local rev; rev=$(git -C "$TP/sqlite" rev-parse HEAD)
  if [ -f "$SQLITE_BLD/sqlite3.c" ] && [ "$(cat "$SQLITE_BLD/.rev" 2>/dev/null)" = "$rev" ]; then
    return
  fi
  command -v tclsh >/dev/null || { echo "tclsh is needed to generate sqlite3.c" >&2; exit 1; }
  echo "  generate sqlite3.c from third_party/sqlite @ ${rev:0:10}"
  rm -rf "$SQLITE_BLD"; mkdir -p "$SQLITE_BLD"
  ( cd "$SQLITE_BLD" &&
    CC=$HOST_CC "$TP/sqlite/configure" --disable-tcl > configure.log 2>&1 &&
    make sqlite3.c > make.log 2>&1 ) || {
    echo "sqlite3.c generation failed, see $SQLITE_BLD/{configure,make}.log" >&2; exit 1; }
  echo "$rev" > "$SQLITE_BLD/.rev"
}

# --- FFmpeg + x264 libraries -------------------------------------------------
# libavcodec with only the codecs below, single-threaded and C only, so the
# wasm and the native build run the same code. Built per target into
# build/ffmpeg-<wasm|native> and rebuilt when a submodule or the flags change.
FF_ENCODERS="libx264 mpeg4 mpeg2video mjpeg prores_ks ffv1"
FF_DECODERS="h264 mpeg4 mpeg2video mjpeg prores ffv1"
FF_PARSERS="h264"

FF_WASM_CFLAGS="-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"
FF_WASM_LIBS="-lwasi-emulated-signal -lwasi-emulated-process-clocks"

# Toolchain for one target, set by ffmpeg_toolchain and used by the two
# library builds below.
FF_CC=""
FF_AR=""
FF_RANLIB=""
FF_NM=""
FF_CFLAGS=""

ffmpeg_toolchain() {
  if [ "$1" = wasm ]; then
    FF_CC=$CC
    FF_AR=$WASI_SDK/bin/llvm-ar
    FF_RANLIB=$WASI_SDK/bin/llvm-ranlib
    FF_NM=$WASI_SDK/bin/llvm-nm
    FF_CFLAGS="$OPT $FF_WASM_CFLAGS"
  else
    FF_CC=$HOST_CC
    FF_AR=ar
    FF_RANLIB=ranlib
    FF_NM=nm
    FF_CFLAGS="-O2"
  fi
}

build_x264() {
  local target=$1
  local prefix=$2

  local host_flags=()
  if [ "$target" = wasm ]; then
    host_flags=(--host=wasm32-unknown-linux-gnu)
  fi

  cd "$prefix/x264"

  CC=$FF_CC AR=$FF_AR RANLIB=$FF_RANLIB \
    "$TP/x264/configure" "${host_flags[@]}" \
      --prefix="$prefix" \
      --enable-static \
      --disable-cli \
      --disable-asm \
      --disable-thread \
      --disable-opencl \
      --extra-cflags="$FF_CFLAGS" \
      > configure.log 2>&1 || return 1

  # wasi-libc has no malloc.h.
  if [ "$target" = wasm ]; then
    sed -i 's/#define HAVE_MALLOC_H 1/#define HAVE_MALLOC_H 0/' config.h
  fi

  make -j"$(nproc)" > make.log 2>&1 || return 1
  make install-lib-static > /dev/null
}

build_libavcodec() {
  local target=$1
  local prefix=$2

  local cross_flags=()
  if [ "$target" = wasm ]; then
    cross_flags=(--enable-cross-compile --target-os=none --arch=wasm32 --disable-simd128)
  fi

  local codec_flags=()
  local name
  for name in $FF_ENCODERS; do codec_flags+=(--enable-encoder="$name"); done
  for name in $FF_DECODERS; do codec_flags+=(--enable-decoder="$name"); done
  for name in $FF_PARSERS;  do codec_flags+=(--enable-parser="$name");  done

  cd "$prefix/ffmpeg"

  PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig \
    "$TP/ffmpeg/configure" "${cross_flags[@]}" \
      --prefix="$prefix" \
      --cc="$FF_CC" \
      --ar="$FF_AR" \
      --ranlib="$FF_RANLIB" \
      --nm="$FF_NM" \
      --extra-cflags="$FF_CFLAGS -I$prefix/include" \
      --extra-ldflags="-L$prefix/lib" \
      --pkg-config=pkg-config \
      --pkg-config-flags=--static \
      --disable-everything \
      --disable-programs \
      --disable-doc \
      --disable-debug \
      --disable-autodetect \
      --disable-pthreads \
      --disable-w32threads \
      --disable-os2threads \
      --disable-asm \
      --disable-inline-asm \
      --disable-network \
      --disable-avformat \
      --disable-avfilter \
      --disable-avdevice \
      --disable-swresample \
      --disable-swscale \
      --enable-gpl \
      --enable-libx264 \
      "${codec_flags[@]}" \
      > configure.log 2>&1 || return 1

  make -j"$(nproc)" > make.log 2>&1 || return 1
  make install > /dev/null
}

ffmpeg_libs() {
  local target=$1
  local prefix=$BLD/ffmpeg-$target

  ffmpeg_toolchain "$target"

  local key
  key="$(git -C "$TP/ffmpeg" rev-parse HEAD) $(git -C "$TP/x264" rev-parse HEAD)"
  key="$key $FF_CC $FF_CFLAGS $FF_ENCODERS $FF_DECODERS $FF_PARSERS"
  if [ "$(cat "$prefix/.key" 2>/dev/null)" = "$key" ]; then
    return
  fi

  echo "  build libavcodec + x264 ($target)"
  rm -rf "$prefix"
  mkdir -p "$prefix/x264" "$prefix/ffmpeg"

  if ! (build_x264 "$target" "$prefix"); then
    echo "x264 build failed, see $prefix/x264/{configure,make}.log" >&2
    exit 1
  fi
  if ! (build_libavcodec "$target" "$prefix"); then
    echo "FFmpeg build failed, see $prefix/ffmpeg/{configure,make}.log" >&2
    exit 1
  fi

  echo "$key" > "$prefix/.key"
}

# The source clip (workloads/media) goes into the module as a C array, since
# the modules read no files.
ffmpeg_video() {
  mkdir -p "$BLD/media"
  python3 -I - "$WL/media/noise_160x90.h264" "$BLD/media/ffmpeg_video.h" <<'PY'
import sys

src, dst = sys.argv[1:3]
data = open(src, "rb").read()

rows = []
for i in range(0, len(data), 24):
    rows.append(",".join(str(b) for b in data[i:i + 24]))

with open(dst, "w") as f:
    f.write("static const unsigned char ffmpeg_video[] = {\n")
    f.write(",\n".join(rows))
    f.write("\n};\n")
PY
}

ffmpeg_link() {
  local prefix=$BLD/ffmpeg-$1
  echo "-I $prefix/include -L $prefix/lib -lavcodec -lx264 -lavutil -lm"
  if [ "$1" = wasm ]; then
    echo "$FF_WASM_LIBS"
  fi
}

# Flags + sources for one workload, shared by the wasm and the native build so
# the two can only differ in target.
workload_args() {
  case $1 in
  sqlite) echo "$SQLFLAGS -I $SQLITE_BLD $WL/sqlite.c $SQLITE_BLD/sqlite3.c" ;;
  json)   echo "-I $TP/cjson $WL/json.c $TP/cjson/cJSON.c" ;;
  btree)  echo "-I $TP/btree $WL/btree.c $TP/btree/btree.c" ;;
  xml)    echo "-DEZXML_NOMMAP -I $TP/ezxml $WL/xml.c $TP/ezxml/ezxml.c" ;;
  zlib)   echo "-I $TP/zlib $WL/zlib.c $(for f in $ZLIB_SRCS; do printf '%s ' "$TP/zlib/$f.c"; done)" ;;
  lua)    echo "$LUAFLAGS -I $TP/lua $WL/lua.c $TP/lua/onelua.c" ;;
  ffmpeg) echo "-I $BLD/media $WL/ffmpeg.c" ;;
  esac
}

defs_for() { local v=DEFS_$1; echo "$DEFS ${!v:-}"; }

# Every value set below is a single shell word, so plain word splitting of
# workload_args is safe; the Lua -D values contain parentheses but no spaces.
build_wasm() {
  local w=$1 extra=""
  [ "$w" = lua ] && extra=$LUA_SIGNAL_LIB
  [ "$w" = ffmpeg ] && extra=$(ffmpeg_link wasm)
  # -I $WL first so workloads/setjmp.h shadows the sysroot header for Lua.
  $CC $OPT -Wno-everything -I "$WL" $(defs_for "$w") $(workload_args "$w") \
      -o "$OUT/$w.wasm" $extra $TLDFLAGS
}

build_native() {
  local w=$1 extra=""
  [ "$w" = ffmpeg ] && extra=$(ffmpeg_link native)
  $HOST_CC -O2 -w -I "$WL" $(defs_for "$w") $(workload_args "$w") -o "$BLD/native/$w" $extra -lm
}

echo "toolchain: $TOOLCHAIN ($($CC --version 2>&1 | head -1))"
echo "flags:     $OPT ${DEFS}"
declare -A EXPECT
for w in $WORKLOADS; do
  [ "$w" = sqlite ] && sqlite_amalgamation
  if [ "$w" = ffmpeg ]; then
    ffmpeg_libs wasm
    ffmpeg_libs native
    ffmpeg_video
  fi
  printf '  build %-7s' "$w"
  build_wasm "$w"
  build_native "$w"
  EXPECT[$w]=$("$BLD/native/$w")
  printf '%9s bytes  expected %-10s %s\n' "$(stat -c%s "$OUT/$w.wasm")" "${EXPECT[$w]}" "$(defs_for "$w" | xargs)"
done

# expected.json / MANIFEST.tsv: merge, so building a subset keeps the others.
python3 -I - "$OUT" "$TOOLCHAIN" "$OPT" <<PY
import json, hashlib, os, sys
out, tc, opt = sys.argv[1:4]
new = {$(for w in $WORKLOADS; do printf '"%s": (%s, """%s"""), ' "$w" "${EXPECT[$w]}" "$(defs_for "$w" | xargs)"; done)}
ej = os.path.join(out, "expected.json")
exp = json.load(open(ej)) if os.path.exists(ej) else {}
mf = os.path.join(out, "MANIFEST.tsv")
rows = {}
if os.path.exists(mf):
    for line in open(mf).read().splitlines()[1:]:
        rows[line.split("\t")[0]] = line
for w, (val, defs) in new.items():
    exp[w] = val
    data = open(os.path.join(out, w + ".wasm"), "rb").read()
    rows[w] = "\t".join([w, tc, opt, defs, str(len(data)), hashlib.sha256(data).hexdigest(), str(val)])
json.dump(dict(sorted(exp.items())), open(ej, "w"), indent=2)
open(ej, "a").write("\n")
with open(mf, "w") as f:
    f.write("workload\ttoolchain\topt\tdefs\tbytes\tsha256\texpected\n")
    for w in sorted(rows): f.write(rows[w] + "\n")
PY

[ "$VERIFY" = 1 ] || exit 0
if [ ! -x "$WALRUS" ]; then
  echo "walrus not found at $WALRUS — set WALRUS=... to verify (skipping)"; exit 0
fi

# One JIT run each against the native result. benchmark.py checks every run of
# every mode again; this is only to fail the build early.
echo "verify on $WALRUS --jit"
fail=0
for w in $WORKLOADS; do
  got=$("$WALRUS" --jit "$OUT/$w.wasm" 2>&1 | tail -1)
  if [ "$got" = "${EXPECT[$w]}" ]; then printf '  ok   %-7s %s\n' "$w" "$got"
  else fail=1; printf '  FAIL %-7s expected %s, got %s\n' "$w" "${EXPECT[$w]}" "$got"; fi
done
exit $fail

#!/usr/bin/env python3
"""Build the WASMBench workloads in containers.

    ./build.py                      every workload enabled in config.json
    ./build.py sqlite zlib          only these
    ./build.py --config my.json     another configuration

Each workload builds in its own image (container/<name>.Containerfile) on a
shared toolchain image (container/base.Containerfile); the host needs only
podman (preferred when both are installed) or docker. The same script runs inside the images (--inside) to compile
the workload twice from the same source and -D flags: to wasm32-wasip1 with
wasi-sdk, and natively with gcc. The native run's output is the expected value.
The results are collected into the output directory:

    <name>.wasm      the module; runs without arguments, prints one number
    expected.json    {workload: expected value}
    MANIFEST.tsv     compile flags, defines, size, sha256 and expected value

Running the modules is left to the benchmark runner: whatever the engine, a
module's output must equal its value in expected.json.

config.json
    output     output directory, relative to this directory
    compile    wasm compile options for every workload:
                 opt      optimisation level, e.g. "-O2", "-O3", "-Os"
                 cflags   extra wasm compiler flags, e.g. ["-msimd128"]; also
                          used for ffmpeg's libraries. Not for -D: knobs go in
                          defines, so the native reference build sees them too
                 ldflags  extra wasm linker flags, e.g. ["-Wl,-z,stack-size=1048576"]
    workloads  {name: {"enabled": bool, "compile": {...}, "defines": {...}}}
                 compile  per-workload override: opt replaces the global one,
                          cflags/ldflags are appended to the global ones
                 defines  -D knobs: {"ROWS": 5000} -> -DROWS=5000; null leaves
                          the knob at the driver's default (for defaults that
                          depend on another knob). The knobs and their defaults
                          are documented at the top of workloads/<name>.c

Python 3.8+, standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
CONTAINER = os.path.join(ROOT, "container")

# Submodules each workload needs (third_party/<dir>).
SOURCES = {
    "sqlite": ["sqlite"], "json": ["cjson"], "btree": ["btree"], "xml": ["ezxml"],
    "zlib": ["zlib"], "lua": ["lua"], "ffmpeg": ["ffmpeg", "x264"],
}


def die(msg):
    sys.exit(f"build.py: {msg}")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def define_flags(defines):
    """{"ROWS": 5000} -> ["-DROWS=5000"]; null keeps the driver's default."""
    out = []
    for k, v in defines.items():
        if v is None:
            continue
        out.append(f"-D{k}" if v is True else f"-D{k}={v}")
    return out


# ---------------------------------------------------------------------------
# Host side: configuration, images, collection, verification
# ---------------------------------------------------------------------------

CONFIG_KEYS = {"output", "compile", "workloads"}
WORKLOAD_KEYS = {"enabled", "compile", "defines"}
COMPILE_KEYS = {"opt", "cflags", "ldflags"}


def load_config(path):
    try:
        with open(path) as f:
            cfg = json.load(f)
    except (OSError, ValueError) as e:
        die(f"{path}: {e}")
    def check_keys(obj, allowed, where):
        if not isinstance(obj, dict):
            die(f"{path}: {where} must be an object")
        if set(obj) - allowed:
            die(f"{path}: {where}: unknown keys {sorted(set(obj) - allowed)}")

    def check_compile(c, where):
        check_keys(c, COMPILE_KEYS, where)
        for k in ("cflags", "ldflags"):
            if not isinstance(c.get(k, []), list) or not all(isinstance(x, str) for x in c.get(k, [])):
                die(f"{path}: {where}.{k} must be a list of strings")

    check_keys(cfg, CONFIG_KEYS, "top level")
    check_compile(cfg.get("compile", {}), "compile")
    for name, wl in cfg.get("workloads", {}).items():
        if name not in SOURCES:
            die(f"{path}: unknown workload '{name}' (known: {' '.join(SOURCES)})")
        check_keys(wl, WORKLOAD_KEYS, f"workloads.{name}")
        check_compile(wl.get("compile", {}), f"workloads.{name}.compile")
        check_keys(wl.get("defines", {}), set(wl.get("defines", {})), f"workloads.{name}.defines")
    return cfg


def compile_options(cfg, name):
    """Global compile options with the workload's own on top."""
    g, w = cfg.get("compile", {}), cfg.get("workloads", {}).get(name, {}).get("compile", {})
    return (w.get("opt", g.get("opt", "-O2")),
            g.get("cflags", []) + w.get("cflags", []),
            g.get("ldflags", []) + w.get("ldflags", []))


def pick_engine():
    for e in ("podman", "docker"):
        if shutil.which(e):
            return e
    die("neither podman nor docker found")


def image_exists(engine, tag):
    return subprocess.run([engine, "image", "inspect", tag],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def build_base(engine):
    # Tagged by the Containerfile's content: rebuilt only when that file changes.
    with open(os.path.join(CONTAINER, "base.Containerfile"), "rb") as f:
        tag = "wasmbench-base:" + hashlib.sha256(f.read()).hexdigest()[:12]
    if not image_exists(engine, tag):
        print(f"== base image {tag}", flush=True)
        run([engine, "build", "-t", tag, "-f", os.path.join(CONTAINER, "base.Containerfile"), CONTAINER])
    return tag


def build_workload(engine, base, name, opt, cflags, ldflags, defs, tmp):
    """Build wasmbench-<name> and copy its /out to tmp/<name>; returns that dir."""
    print(f"== {name}  {' '.join([opt] + cflags + ldflags + defs)}", flush=True)
    tag = f"wasmbench-{name}:latest"
    run([engine, "build", "-t", tag, "-f", os.path.join(CONTAINER, f"{name}.Containerfile"),
         "--build-arg", f"BASE={base}",
         "--build-arg", f"WASMBENCH_OPT={opt}",
         # Lists go as JSON so every flag reaches the compiler as one argument.
         # Prefixed: build args become environment variables in the image, and
         # x264/FFmpeg's configure would read a plain CFLAGS/LDFLAGS.
         "--build-arg", f"WASMBENCH_CFLAGS={json.dumps(cflags)}",
         "--build-arg", f"WASMBENCH_LDFLAGS={json.dumps(ldflags)}",
         "--build-arg", f"WASMBENCH_DEFS={json.dumps(defs)}",
         ROOT])
    cid = run([engine, "create", tag], capture_output=True, text=True).stdout.strip()
    dst = os.path.join(tmp, name)
    try:
        run([engine, "cp", f"{cid}:/out", dst], stdout=subprocess.DEVNULL)
    finally:
        subprocess.run([engine, "rm", cid], stdout=subprocess.DEVNULL)
    return dst


def collect(out, name, src):
    """Merge one workload's result into out/: .wasm, expected.json, MANIFEST.tsv."""
    with open(os.path.join(src, f"{name}.json")) as f:
        info = json.load(f)
    wasm = os.path.join(out, f"{name}.wasm")
    shutil.copyfile(os.path.join(src, f"{name}.wasm"), wasm)
    with open(wasm, "rb") as f:
        data = f.read()

    ej = os.path.join(out, "expected.json")
    expected = {}
    if os.path.exists(ej):
        with open(ej) as f:
            expected = json.load(f)
    expected[name] = info["expected"]
    with open(ej, "w") as f:
        json.dump(dict(sorted(expected.items())), f, indent=2)
        f.write("\n")

    mf = os.path.join(out, "MANIFEST.tsv")
    rows = {}
    if os.path.exists(mf):
        with open(mf) as f:
            for line in f.read().splitlines()[1:]:
                rows[line.split("\t")[0]] = line
    rows[name] = "\t".join([name, info["toolchain"], info["opt"], " ".join(info["cflags"]),
                            " ".join(info["ldflags"]), " ".join(info["defs"]),
                            str(len(data)), hashlib.sha256(data).hexdigest(), str(info["expected"])])
    with open(mf, "w") as f:
        f.write("workload\ttoolchain\topt\tcflags\tldflags\tdefs\tbytes\tsha256\texpected\n")
        for k in sorted(rows):
            f.write(rows[k] + "\n")
    print(f"   {name}.wasm  {len(data)} bytes  expected {info['expected']}", flush=True)


def host_main(args):
    cfg = load_config(args.config)
    wl_cfg = cfg.get("workloads", {})
    names = args.workloads or [n for n in SOURCES if wl_cfg.get(n, {}).get("enabled", True)]
    for n in names:
        if n not in SOURCES:
            die(f"unknown workload '{n}' (known: {' '.join(SOURCES)})")
        for d in SOURCES[n]:
            p = os.path.join(ROOT, "third_party", d)
            if not os.path.isdir(p) or not os.listdir(p):
                die(f"third_party/{d} is empty; run: git submodule update --init --depth 1")

    engine = pick_engine()
    out = os.path.join(ROOT, cfg.get("output", "artifacts"))
    os.makedirs(out, exist_ok=True)
    base = build_base(engine)
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            opt, cflags, ldflags = compile_options(cfg, n)
            defs = define_flags(wl_cfg.get(n, {}).get("defines", {}))
            collect(out, n, build_workload(engine, base, n, opt, cflags, ldflags, defs, tmp))
    return 0


# ---------------------------------------------------------------------------
# Container side (--inside): runs in container/<name>.Containerfile
# ---------------------------------------------------------------------------

TP = os.path.join(ROOT, "third_party")
WL = os.path.join(ROOT, "workloads")
OUT = os.environ.get("OUT", "/out")
BLD = os.environ.get("BLD", "/build")
WASI_SDK = os.environ.get("WASI_SDK", "/opt/wasi-sdk")
HOST_CC = os.environ.get("HOST_CC", "gcc")
WASI_CC = os.path.join(WASI_SDK, "bin", "clang")   # clang.cfg selects wasm32-wasip1

# SQLite: drop everything that is impossible on wasip1 or that would measure
# something other than the query engine. The driver opens ":memory:".
SQLITE_FLAGS = ["-DSQLITE_THREADSAFE=0", "-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_OMIT_WAL",
                "-DSQLITE_TEMP_STORE=3", "-DSQLITE_OMIT_DEPRECATED", "-DSQLITE_DEFAULT_MEMSTATUS=0"]

# Lua freestanding on wasm without the exception-handling proposal:
#   luai_makeseed()=0u   fixed string-hash seed (otherwise stack address + clock)
#   LUAI_THROW/LUAI_TRY  no setjmp/longjmp; a script error aborts loudly
#   workloads/setjmp.h   shadows the sysroot's #error header (-I workloads first)
#   L_tmpnam + stubs     loslib's hosted-C references (see workloads/lua.c)
LUA_FLAGS = ["-DMAKE_LIB", "-Dluai_makeseed()=0u", "-DL_tmpnam=32", "-D_WASI_EMULATED_SIGNAL",
             "-Dluai_jmpbuf=int", "-DLUAI_THROW(L,c)=abort()", "-DLUAI_TRY(L,c,f,ud)=((f)(L,ud))"]

ZLIB_SRCS = ["adler32", "crc32", "deflate", "inflate", "inftrees", "inffast", "trees", "zutil",
             "compress", "uncompr"]

# libavcodec with only these codecs, single-threaded and C only, so the wasm
# and the native build run the same code.
FF_ENCODERS = ["libx264", "mpeg4", "mpeg2video", "mjpeg", "prores_ks", "ffv1"]
FF_DECODERS = ["h264", "mpeg4", "mpeg2video", "mjpeg", "prores", "ffv1"]
FF_PARSERS = ["h264"]
FF_WASM_CFLAGS = ["-D_WASI_EMULATED_SIGNAL", "-D_WASI_EMULATED_PROCESS_CLOCKS"]
FF_WASM_LIBS = ["-lwasi-emulated-signal", "-lwasi-emulated-process-clocks"]


def logged(cmd, log, cwd=None, env=None):
    with open(log, "w") as f:
        if subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode:
            die(f"{' '.join(cmd[:2])} failed, see {log}")


def stamp_ok(d, key):
    try:
        with open(os.path.join(d, ".key")) as f:
            return f.read() == key
    except OSError:
        return False


def write_stamp(d, key):
    with open(os.path.join(d, ".key"), "w") as f:
        f.write(key)


def sqlite_amalgamation():
    """Generate sqlite3.c from the sqlite/sqlite source tree (configure + make +
    tclsh). From tag version-3.45.1 it is identical to the sqlite.org release
    amalgamation except for the 4-line provenance comment."""
    d = os.path.join(BLD, "sqlite")
    if stamp_ok(d, "sqlite3.c"):
        return
    print("  generate sqlite3.c", flush=True)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    logged([os.path.join(TP, "sqlite", "configure"), "--disable-tcl"], os.path.join(d, "configure.log"),
           cwd=d, env={**os.environ, "CC": HOST_CC})
    logged(["make", "sqlite3.c"], os.path.join(d, "make.log"), cwd=d)
    write_stamp(d, "sqlite3.c")


def ffmpeg_libs(target, opt, cflags):
    """x264 + libavcodec for one target into BLD/ffmpeg-<target>."""
    prefix = os.path.join(BLD, f"ffmpeg-{target}")
    if target == "wasm":
        cc, ar, ranlib, nm = (WASI_CC, *(os.path.join(WASI_SDK, "bin", t)
                                         for t in ("llvm-ar", "llvm-ranlib", "llvm-nm")))
        cflags = " ".join([opt] + cflags + FF_WASM_CFLAGS)
    else:
        cc, ar, ranlib, nm, cflags = HOST_CC, "ar", "ranlib", "nm", "-O2"
    key = " ".join([cc, cflags] + FF_ENCODERS + FF_DECODERS + FF_PARSERS)
    if stamp_ok(prefix, key):
        return
    print(f"  build libavcodec + x264 ({target})", flush=True)
    shutil.rmtree(prefix, ignore_errors=True)
    x264, ff = os.path.join(prefix, "x264"), os.path.join(prefix, "ffmpeg")
    os.makedirs(x264)
    os.makedirs(ff)
    jobs = f"-j{os.cpu_count() or 1}"

    logged([os.path.join(TP, "x264", "configure")]
           + (["--host=wasm32-unknown-linux-gnu"] if target == "wasm" else [])
           + [f"--prefix={prefix}", "--enable-static", "--disable-cli", "--disable-asm",
              "--disable-thread", "--disable-opencl", f"--extra-cflags={cflags}"],
           os.path.join(x264, "configure.log"), cwd=x264,
           env={**os.environ, "CC": cc, "AR": ar, "RANLIB": ranlib})
    if target == "wasm":   # wasi-libc has no malloc.h
        cfg = os.path.join(x264, "config.h")
        with open(cfg) as f:
            text = f.read()
        with open(cfg, "w") as f:
            f.write(text.replace("#define HAVE_MALLOC_H 1", "#define HAVE_MALLOC_H 0"))
    logged(["make", jobs], os.path.join(x264, "make.log"), cwd=x264)
    logged(["make", "install-lib-static"], os.path.join(x264, "install.log"), cwd=x264)

    logged([os.path.join(TP, "ffmpeg", "configure")]
           + (["--enable-cross-compile", "--target-os=none", "--arch=wasm32", "--disable-simd128"]
              if target == "wasm" else [])
           + [f"--prefix={prefix}", f"--cc={cc}", f"--ar={ar}", f"--ranlib={ranlib}", f"--nm={nm}",
              f"--extra-cflags={cflags} -I{prefix}/include", f"--extra-ldflags=-L{prefix}/lib",
              "--pkg-config=pkg-config", "--pkg-config-flags=--static",
              "--disable-everything", "--disable-programs", "--disable-doc", "--disable-debug",
              "--disable-autodetect", "--disable-pthreads", "--disable-w32threads",
              "--disable-os2threads", "--disable-asm", "--disable-inline-asm", "--disable-network",
              "--disable-avformat", "--disable-avfilter", "--disable-avdevice",
              "--disable-swresample", "--disable-swscale", "--enable-gpl", "--enable-libx264"]
           + [f"--enable-encoder={n}" for n in FF_ENCODERS]
           + [f"--enable-decoder={n}" for n in FF_DECODERS]
           + [f"--enable-parser={n}" for n in FF_PARSERS],
           os.path.join(ff, "configure.log"), cwd=ff,
           env={**os.environ, "PKG_CONFIG_LIBDIR": os.path.join(prefix, "lib", "pkgconfig")})
    logged(["make", jobs], os.path.join(ff, "make.log"), cwd=ff)
    logged(["make", "install"], os.path.join(ff, "install.log"), cwd=ff)
    write_stamp(prefix, key)


def ffmpeg_video():
    """The source clip as a C array: the modules read no files."""
    d = os.path.join(BLD, "media")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(WL, "media", "noise_160x90.h264"), "rb") as f:
        data = f.read()
    rows = [",".join(str(b) for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    with open(os.path.join(d, "ffmpeg_video.h"), "w") as f:
        f.write("static const unsigned char ffmpeg_video[] = {\n" + ",\n".join(rows) + "\n};\n")


def prepare(name, opt, cflags):
    """Generated sources and prebuilt libraries. --prepare stops after this, so
    a Containerfile can cache it in a layer below the -D knobs."""
    if name == "sqlite":
        sqlite_amalgamation()
    elif name == "ffmpeg":
        ffmpeg_libs("wasm", opt, cflags)
        ffmpeg_libs("native", opt, cflags)
        ffmpeg_video()


def workload_args(name):
    """Flags + sources, shared by the wasm and the native build so the two can
    only differ in target."""
    t = lambda *p: os.path.join(TP, *p)
    w = lambda f: os.path.join(WL, f)
    sq = os.path.join(BLD, "sqlite")
    return {
        "sqlite": SQLITE_FLAGS + ["-I", sq, w("sqlite.c"), os.path.join(sq, "sqlite3.c")],
        "json": ["-I", t("cjson"), w("json.c"), t("cjson", "cJSON.c")],
        "btree": ["-I", t("btree"), w("btree.c"), t("btree", "btree.c")],
        "xml": ["-DEZXML_NOMMAP", "-I", t("ezxml"), w("xml.c"), t("ezxml", "ezxml.c")],
        "zlib": ["-I", t("zlib"), w("zlib.c")] + [t("zlib", f + ".c") for f in ZLIB_SRCS],
        "lua": LUA_FLAGS + ["-I", t("lua"), w("lua.c"), t("lua", "onelua.c")],
        "ffmpeg": ["-I", os.path.join(BLD, "media"), w("ffmpeg.c")],
    }[name]


def link_args(name, target):
    if name == "lua" and target == "wasm":
        return ["-lwasi-emulated-signal"]
    if name == "ffmpeg":
        p = os.path.join(BLD, f"ffmpeg-{target}")
        # 1 MB stack: wasm-ld's default 64 KB overflows in some encoders (ffv1
        # encode, measured), which a native run with its 8 MB stack never shows.
        return (["-I", f"{p}/include", "-L", f"{p}/lib", "-lavcodec", "-lx264", "-lavutil", "-lm"]
                + (FF_WASM_LIBS + ["-Wl,-z,stack-size=1048576"] if target == "wasm" else []))
    return []


def inside_main(args):
    name = args.inside
    opt = os.environ.get("WASMBENCH_OPT") or "-O2"
    cflags = json.loads(os.environ.get("WASMBENCH_CFLAGS") or "[]")
    ldflags = json.loads(os.environ.get("WASMBENCH_LDFLAGS") or "[]")
    defs = json.loads(os.environ.get("WASMBENCH_DEFS") or "[]")
    prepare(name, opt, cflags)
    if args.prepare:
        return 0

    os.makedirs(OUT, exist_ok=True)
    os.makedirs(os.path.join(BLD, "native"), exist_ok=True)
    wasm = os.path.join(OUT, f"{name}.wasm")
    native = os.path.join(BLD, "native", name)
    # -I workloads first so workloads/setjmp.h shadows the sysroot header for Lua.
    run([WASI_CC, opt, "-Wno-everything"] + cflags + ["-I", WL] + defs + workload_args(name)
        + ["-o", wasm] + link_args(name, "wasm") + ldflags)
    run([HOST_CC, "-O2", "-w", "-I", WL] + defs + workload_args(name)
        + ["-o", native] + link_args(name, "native") + ["-lm"])
    expected = int(run([native], capture_output=True, text=True).stdout.strip().splitlines()[-1])

    version = run([WASI_CC, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    with open(os.path.join(OUT, f"{name}.json"), "w") as f:
        json.dump({"expected": expected, "opt": opt, "cflags": cflags, "ldflags": ldflags, "defs": defs,
                   "toolchain": version.split(" (")[0]}, f)
    print(f"  {name}: {os.path.getsize(wasm)} bytes, expected {expected}", flush=True)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("workloads", nargs="*", help="workloads to build (default: enabled in config)")
    p.add_argument("--config", default=os.path.join(ROOT, "config.json"), help="default: config.json")
    p.add_argument("--inside", metavar="NAME", help=argparse.SUPPRESS)
    p.add_argument("--prepare", action="store_true", help=argparse.SUPPRESS)
    args = p.parse_args()
    try:
        sys.exit(inside_main(args) if args.inside else host_main(args))
    except subprocess.CalledProcessError as e:
        die(f"command failed ({e.returncode}): {' '.join(map(str, e.cmd))}")


if __name__ == "__main__":
    main()

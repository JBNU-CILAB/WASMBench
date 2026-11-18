/* zlib (madler/zlib) deflate / inflate over generated prose.
 *
 * Input is prose with skewed word frequencies, not random bytes: random input
 * gives deflate no matches, so longest_match would never run and inflate would
 * decode almost nothing but literals.
 *
 * Each call does its own deflateInit2/deflateEnd (as compress2 does), so the
 * allocator traffic of a real one-shot caller is part of the measurement. With
 * the default knobs the stream is identical to compress2(6).
 *
 * Knobs (-D):
 *   ITERS     32         measured iterations
 *   BYTES     256*1024   generated input size
 *   LEVEL     6          deflate level 0..9
 *   WBITS     15         log2 of the LZ77 window, 9..15
 *   MEMLEVEL  8          deflate memory level 1..9
 *   STRATEGY  0          0 default, 1 filtered, 2 huffman-only, 3 rle, 4 fixed
 *   MODE      0          0 roundtrip: deflate + inflate + compare each iteration
 *                          (checksum over the compressed stream)
 *                        1 deflate only (checksum over the compressed stream)
 *                        2 inflate only, stream compressed once in setup
 *                          (checksum over the decompressed output)
 *   SEED      0x9E3779B9 input RNG seed (CORPUS_SEED_TRAIN for the train input)
 */
#define BENCH_NAME "zlib"
#include "bench.h"
#include "zlib.h"

#ifndef ITERS
#define ITERS 32
#endif
#ifndef BYTES
#define BYTES (256 * 1024)
#endif
#ifndef LEVEL
#define LEVEL 6
#endif
#ifndef WBITS
#define WBITS 15
#endif
#ifndef MEMLEVEL
#define MEMLEVEL 8
#endif
#ifndef STRATEGY
#define STRATEGY 0
#endif
#ifndef MODE
#define MODE 0
#endif

static uLong zdeflate(Bytef *dst, uLong cap, const Bytef *src, uLong n) {
    z_stream s;
    memset(&s, 0, sizeof s);
    /* Z_DEFAULT_STRATEGY..Z_FIXED are 0..4. */
    if (deflateInit2(&s, LEVEL, Z_DEFLATED, WBITS, MEMLEVEL, STRATEGY) != Z_OK)
        bench_fail("deflateInit2");
    s.next_in = (Bytef *)src; s.avail_in = (uInt)n;
    s.next_out = dst;         s.avail_out = (uInt)cap;
    if (deflate(&s, Z_FINISH) != Z_STREAM_END) bench_fail("deflate");
    uLong out = s.total_out;
    deflateEnd(&s);
    return out;
}

static uLong zinflate(Bytef *dst, uLong cap, const Bytef *src, uLong n) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, WBITS) != Z_OK) bench_fail("inflateInit2");
    s.next_in = (Bytef *)src; s.avail_in = (uInt)n;
    s.next_out = dst;         s.avail_out = (uInt)cap;
    if (inflate(&s, Z_FINISH) != Z_STREAM_END) bench_fail("inflate");
    uLong out = s.total_out;
    inflateEnd(&s);
    return out;
}

BENCH_EXPORT uint32_t runtime(void) {
    char *src = bench_alloc(BYTES);
    size_t n = make_text(src, BYTES, SEED);
    uLong pcap = compressBound((uLong)n);
    Bytef *packed = bench_alloc(pcap), *out = bench_alloc(n + 64);

    uLong plen = 0;
    if (MODE == 2) plen = zdeflate(packed, pcap, (const Bytef *)src, (uLong)n);

    uint32_t h = 2166136261u;
    for (long it = 0; it < ITERS; it++) {
        if (MODE != 2) {
            plen = zdeflate(packed, pcap, (const Bytef *)src, (uLong)n);
            h = fold32(h, packed, plen);
        }
        if (MODE != 1) {
            uLong olen = zinflate(out, (uLong)n + 64, packed, plen);
            if (olen != n || memcmp(out, src, n) != 0) bench_fail("roundtrip mismatch");
            if (MODE == 2) h = fold32(h, out, olen);
        }
    }
    free(src); free(packed); free(out);
    return h;
}

BENCH_MAIN

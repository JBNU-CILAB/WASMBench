/* ezXML over a generated document.
 *
 * ezxml_parse_str() parses in place (it writes NUL terminators and decodes
 * entities into the buffer), so every iteration parses a fresh copy of the
 * pristine document. The memcpy is a small, fixed share of each iteration.
 *
 * The tree is then walked in document order and every tag name, attribute and
 * text run is folded into the checksum.
 *
 * Built with EZXML_NOMMAP: the workload never touches a file.
 *
 * Knobs (-D):
 *   ITERS   256        parse + walk (+ print) passes
 *   BYTES   192*1024   generated document size (>= 1024)
 *   DEPTH   7          maximum element nesting, 2..32
 *   ATTRS   60         chance (%) of one more attribute per tag, 0..95
 *   PRINT   0          1: also ezxml_toxml() each iteration, folded in
 *   SEED    0x9E3779B9 input RNG seed (CORPUS_SEED_TRAIN for the train input)
 */
#define BENCH_NAME "xml"
#include "bench.h"
#include "ezxml.h"

#ifndef ITERS
#define ITERS 256
#endif
#ifndef BYTES
#define BYTES (192 * 1024)
#endif
#ifndef DEPTH
#define DEPTH 7
#endif
#ifndef ATTRS
#define ATTRS 60
#endif
#ifndef PRINT
#define PRINT 0
#endif

static uint32_t walk(ezxml_t x, uint32_t h, int d) {
    for (; x; x = x->ordered) {
        h = fold32(h, (const unsigned char *)x->name, strlen(x->name));
        for (char **a = x->attr; a && a[0]; a += 2) {
            h = fold32(h, (const unsigned char *)a[0], strlen(a[0]));
            h = fold32(h, (const unsigned char *)a[1], strlen(a[1]));
        }
        if (x->txt) h = fold32(h, (const unsigned char *)x->txt, strlen(x->txt));
        if (d < 32) h = walk(x->child, h, d + 1);
    }
    return h;
}

BENCH_EXPORT uint32_t runtime(void) {
    /* 1 KB of slack past BYTES for the closing tags make_xml appends after its
     * fill loop (up to DEPTH of them). */
    size_t cap = (size_t)BYTES + 1024;
    char *doc = bench_alloc(cap), *work = bench_alloc(cap);
    size_t n = make_xml(doc, BYTES, SEED, DEPTH, ATTRS);

    uint32_t h = 2166136261u;
    for (long it = 0; it < ITERS; it++) {
        memcpy(work, doc, n + 1);
        ezxml_t x = ezxml_parse_str(work, n);
        if (!x || *ezxml_error(x)) bench_fail("%s", x ? ezxml_error(x) : "null");
        h = walk(x, h, 0);
        if (PRINT) {
            char *s = ezxml_toxml(x);
            h = fold32(h, (const unsigned char *)s, strlen(s));
            free(s);
        }
        ezxml_free(x);
    }
    free(doc); free(work);
    return h;
}

BENCH_MAIN

/* cJSON over a generated document.
 *
 * Each iteration parses the whole document, walks the resulting tree folding
 * every key, string and number into the checksum, and frees it. cJSON is a
 * hand-written recursive-descent parser that classifies characters inline, and
 * the parse/free pair is dominated by malloc/free of small nodes.
 *
 * Knobs (-D):
 *   ITERS   96         parse + walk (+ print) passes
 *   BYTES   192*1024   generated document size (>= 1024)
 *   DEPTH   6          maximum object/array nesting, 1..32
 *   PRINT   0          0: none, 1: cJSON_PrintUnformatted, 2: cJSON_Print
 *                      each iteration, folded into the checksum
 *   SEED    0x9E3779B9 input RNG seed (CORPUS_SEED_TRAIN for the train input)
 */
#define BENCH_NAME "json"
#include "bench.h"
#include "cJSON.h"

#ifndef ITERS
#define ITERS 96
#endif
#ifndef BYTES
#define BYTES (192 * 1024)
#endif
#ifndef DEPTH
#define DEPTH 6
#endif
#ifndef PRINT
#define PRINT 0
#endif

static uint32_t walk(cJSON *x, uint32_t h, int d) {
    for (; x; x = x->next) {
        if (x->string) h = fold32(h, (const unsigned char *)x->string, strlen(x->string));
        if (cJSON_IsString(x) && x->valuestring)
            h = fold32(h, (const unsigned char *)x->valuestring, strlen(x->valuestring));
        else if (cJSON_IsNumber(x)) h = h * 16777619u ^ (uint32_t)x->valueint;
        if (d < 32) h = walk(x->child, h, d + 1);
    }
    return h;
}

BENCH_EXPORT uint32_t runtime(void) {
    char *doc = bench_alloc(BYTES);
    make_json(doc, BYTES, SEED, DEPTH);

    uint32_t h = 2166136261u;
    for (long it = 0; it < ITERS; it++) {
        cJSON *j = cJSON_Parse(doc);
        if (!j) bench_fail("parse error at %.20s", cJSON_GetErrorPtr());
        h = walk(j, h, 0);
        if (PRINT) {
            char *s = (PRINT == 2) ? cJSON_Print(j) : cJSON_PrintUnformatted(j);
            if (!s) bench_fail("print");
            h = fold32(h, (const unsigned char *)s, strlen(s));
            cJSON_free(s);
        }
        cJSON_Delete(j);
    }
    free(doc);
    return h;
}

BENCH_MAIN

/* tidwall/btree.c over generated keys.
 *
 * A B-tree's hot path is a search within each node plus the comparison
 * callback, repeated per level — comparison-heavy and data-dependent, with the
 * split/merge paths taken rarely. Each iteration builds a fresh tree and runs
 * insert, lookup, ordered scan and delete over it.
 *
 * Keys are generated words with a random number appended, so they collide in
 * their prefixes the way real keys do and the comparator has to run past the
 * first byte.
 *
 * Knobs (-D):
 *   ITERS      16     build / query / tear-down cycles
 *   KEYS       30000  rows inserted per iteration
 *   FANOUT     0      max items per node (0 = library default)
 *   LOOKUPS    1      lookup passes over all keys per iteration
 *   DELSTRIDE  3      delete every Nth key (0 = no deletes)
 *   SEED       0x9E3779B9 input RNG seed (CORPUS_SEED_TRAIN for the train input)
 */
#define BENCH_NAME "btree"
#include "bench.h"
#include "btree.h"

#ifndef ITERS
#define ITERS 16
#endif
#ifndef KEYS
#define KEYS 30000
#endif
#ifndef FANOUT
#define FANOUT 0
#endif
#ifndef LOOKUPS
#define LOOKUPS 1
#endif
#ifndef DELSTRIDE
#define DELSTRIDE 3
#endif

#define KEYLEN 24
struct row { char k[KEYLEN]; uint32_t v; };

static int cmp(const void *a, const void *b, void *udata) {
    (void)udata;
    return strncmp(((const struct row *)a)->k, ((const struct row *)b)->k, KEYLEN);
}

static uint32_t h = 2166136261u;
static bool visit(const void *item, void *udata) {
    (void)udata;
    const struct row *r = item;
    h = fold32(h, (const unsigned char *)r->k, strnlen(r->k, KEYLEN));
    h = h * 16777619u ^ r->v;
    return true;
}

BENCH_EXPORT uint32_t runtime(void) {
    struct row *rows = bench_alloc((size_t)KEYS * sizeof *rows);

    corpus_seed(SEED);
    for (long i = 0; i < KEYS; i++) {
        char w[32];
        size_t wl = emit_word(w, 0, sizeof w - 1);
        w[wl] = '\0';
        snprintf(rows[i].k, KEYLEN, "%s-%06u", w, (unsigned)(crnd() % 1000000u));
        rows[i].v = crnd();
    }

    for (long it = 0; it < ITERS; it++) {
        struct btree *t = btree_new(sizeof(struct row), FANOUT, cmp, NULL);
        if (!t) bench_fail("btree_new");
        for (long i = 0; i < KEYS; i++) btree_set(t, &rows[i]);           /* insert + splits */
        for (long l = 0; l < LOOKUPS; l++)
            for (long i = 0; i < KEYS; i++) {                             /* lookup */
                const struct row *r = btree_get(t, &rows[i]);
                if (r) h = h * 16777619u ^ r->v;
            }
        btree_ascend(t, NULL, visit, NULL);                               /* ordered scan */
        if (DELSTRIDE)
            for (long i = 0; i < KEYS; i += (DELSTRIDE ? DELSTRIDE : 1))
                btree_delete(t, &rows[i]);                                /* delete + merges */
        h = h * 16777619u ^ (uint32_t)btree_count(t);
        btree_free(t);
    }
    free(rows);
    return h;
}

BENCH_MAIN

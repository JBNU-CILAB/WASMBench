/* SQLite over an in-memory database.
 *
 * Setup creates one table with two indexes and inserts ROWS rows in a single
 * transaction; the measured loop then runs the queries selected by QUERIES,
 * ITERS times. The SQL tokenizer and parser, the expression walker, the B-tree
 * and the VDBE dispatch loop all run on every statement.
 *
 * Everything runs against ":memory:", so there is no file I/O and no VFS
 * behaviour to depend on (see build.py for the compile-time options). Nothing
 * here calls random(), datetime() or anything else that reads a clock or an
 * entropy source.
 *
 * Knobs (-D):
 *   ITERS     96    passes over the selected queries
 *   ROWS      1800  rows inserted during setup
 *   RANGE     500   column n is drawn from [0, RANGE)
 *   QUERIES   31    bitmask over QUERY_SQL[] (bit i enables query i)
 *   PREPARED  0     0: sqlite3_exec re-parses each query every time
 *                   1: prepare once, then step/reset (no parser in the loop)
 *   SEED      0x9E3779B9 input RNG seed (CORPUS_SEED_TRAIN for the train input)
 */
#define BENCH_NAME "sqlite"
#include "bench.h"
#include "sqlite3.h"

#ifndef ITERS
#define ITERS 96
#endif
#ifndef ROWS
#define ROWS 1800
#endif
#ifndef RANGE
#define RANGE 500
#endif
#ifndef QUERIES
#define QUERIES 31
#endif
#ifndef PREPARED
#define PREPARED 0
#endif

/* A scan with a LIKE, a GROUP BY that sorts, a join, a correlated subquery and
 * an indexed range. */
static const char *const QUERY_SQL[] = {
    "SELECT w, n FROM t WHERE w LIKE 'b%' ORDER BY n DESC, w LIMIT 40;",
    "SELECT w, COUNT(*) c, SUM(n) s FROM t GROUP BY w HAVING c > 1 ORDER BY s DESC, w LIMIT 40;",
    "SELECT a.w, b.w FROM t a JOIN t b ON a.n = b.n + 1 ORDER BY a.w, b.w LIMIT 40;",
    "SELECT w FROM t WHERE n > (SELECT AVG(n) FROM t) ORDER BY w LIMIT 40;",
    "SELECT w, n FROM t WHERE n BETWEEN 100 AND 400 ORDER BY n, w LIMIT 40;",
};
#define NQ ((int)(sizeof(QUERY_SQL) / sizeof(QUERY_SQL[0])))

static uint32_t h = 2166136261u;

static void fold_value(const char *v) {
    if (v) h = fold32(h, (const unsigned char *)v, strlen(v));
    else   h = h * 16777619u ^ 0xFFu;
}

static int cb(void *unused, int ncol, char **val, char **name) {
    (void)unused; (void)name;
    for (int i = 0; i < ncol; i++) fold_value(val[i]);
    return 0;
}

static void run(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, cb, NULL, &err) != SQLITE_OK) bench_fail("%s", err ? err : "?");
}

/* Same rows, same order, same NULL handling as cb(), so PREPARED=0 and
 * PREPARED=1 produce the same checksum. */
static void step_all(sqlite3_stmt *st) {
    int rc, ncol = sqlite3_column_count(st);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        for (int i = 0; i < ncol; i++) fold_value((const char *)sqlite3_column_text(st, i));
    if (rc != SQLITE_DONE) bench_fail("step: %s", sqlite3_errstr(rc));
    sqlite3_reset(st);
}

BENCH_EXPORT uint32_t runtime(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) bench_fail("open");
    run(db, "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;"
            "CREATE TABLE t(id INTEGER PRIMARY KEY, w TEXT, n INTEGER);"
            "CREATE INDEX ix_n ON t(n); CREATE INDEX ix_w ON t(w);");

    /* One transaction: without it every insert is its own, and the benchmark
     * measures commit bookkeeping instead of the query engine. */
    run(db, "BEGIN;");
    corpus_seed(SEED);
    char sql[256], word[64];
    for (long i = 0; i < ROWS; i++) {
        size_t wl = emit_word(word, 0, sizeof word - 1);
        word[wl] = '\0';
        snprintf(sql, sizeof sql, "INSERT INTO t(w,n) VALUES('%s',%u);",
                 word, (unsigned)(crnd() % (uint32_t)RANGE));
        run(db, sql);
    }
    run(db, "COMMIT;");

    sqlite3_stmt *st[NQ] = {0};
    if (PREPARED)
        for (int q = 0; q < NQ; q++)
            if ((QUERIES >> q) & 1)
                if (sqlite3_prepare_v2(db, QUERY_SQL[q], -1, &st[q], NULL) != SQLITE_OK)
                    bench_fail("prepare: %s", sqlite3_errmsg(db));

    for (long it = 0; it < ITERS; it++)
        for (int q = 0; q < NQ; q++) {
            if (!((QUERIES >> q) & 1)) continue;
            if (PREPARED) step_all(st[q]);
            else          run(db, QUERY_SQL[q]);
        }

    for (int q = 0; q < NQ; q++) sqlite3_finalize(st[q]);
    sqlite3_close(db);
    return h;
}

BENCH_MAIN

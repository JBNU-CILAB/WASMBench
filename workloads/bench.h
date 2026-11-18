/* Shared scaffolding, in the shape of walrus' test/wasmBenchmarker ctests:
 * every workload defines
 *
 *     uint32_t runtime(void)
 *
 * which does all the work and returns one checksum, and BENCH_MAIN prints that
 * value as a single decimal line. The module takes no arguments; the runner
 * (benchmark.py) compares the line with the expected value.
 *
 * Tuning is compile time. Each workload declares its knobs as
 *     #ifndef NAME
 *     #define NAME default
 *     #endif
 * and build.sh passes overrides as -D (DEFS / DEFS_<workload>). The expected
 * value for whatever was chosen comes from a native build of the same source
 * with the same -D flags, so a tuned build is checked as strictly as the
 * default one.
 *
 * Every workload has ITERS (how many times the measured loop runs) and SEED
 * (input RNG seed). Failures print to stderr and exit non-zero, so the runner
 * never mistakes an error message for a result.
 */
#ifndef BENCH_H
#define BENCH_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "corpus.h"

#ifndef SEED
#define SEED CORPUS_SEED_TEST
#endif

#if defined(__wasm__)
#define BENCH_EXPORT __attribute__((export_name("runtime")))
#else
#define BENCH_EXPORT
#endif

#define bench_fail(...)                                         \
    do {                                                        \
        fprintf(stderr, "%s: ", BENCH_NAME);                    \
        fprintf(stderr, __VA_ARGS__);                           \
        fputc('\n', stderr);                                    \
        exit(1);                                                \
    } while (0)

static void *bench_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) bench_fail("out of memory allocating %zu bytes", n);
    return p;
}

#define BENCH_MAIN                                              \
    int main(void) {                                            \
        printf("%u\n", (unsigned)runtime());                    \
        return 0;                                               \
    }

#endif

/* Deterministic input generation, shared by every workload.
 *
 * Nothing is read from disk: a run is reproducible to the instruction, and the
 * module needs no WASI file access, no clock and no entropy source.
 *
 * Every generator takes its seed and shape explicitly, so each workload can
 * expose them as compile-time knobs (see bench.h).
 *
 * Random bytes would be the wrong input everywhere here. deflate would find no
 * matches and never enter longest_match, the XML parser would reject at byte
 * one, and a matcher would fail on the first character of every line. So the
 * generators emit plausible structure: skewed word frequencies, nested elements,
 * lines of prose.
 */
#ifndef CORPUS_H
#define CORPUS_H
#include <stdint.h>
#include <stddef.h>

/* The two seeds of the original TRAIN/TEST input families. TEST is the default;
 * -DSEED=CORPUS_SEED_TRAIN (plus that family's shape) reproduces TRAIN. */
#define CORPUS_SEED_TRAIN 0x2545F491u
#define CORPUS_SEED_TEST  0x9E3779B9u

static uint32_t corpus_rng;

static void corpus_seed(uint32_t seed) {
    corpus_rng = seed ? seed : CORPUS_SEED_TEST;   /* xorshift must not start at 0 */
}
static uint32_t crnd(void) {
    corpus_rng ^= corpus_rng << 13;
    corpus_rng ^= corpus_rng >> 17;
    corpus_rng ^= corpus_rng << 5;
    return corpus_rng;
}

static const char *const VOCAB[] = {
    "the","of","and","to","in","a","is","that","for","it","as","was","with",
    "be","by","on","not","he","this","are","or","his","from","at","which","but",
    "have","an","had","they","you","were","their","one","all","we","can","her",
    "has","there","been","if","more","when","will","would","who","so","no","said",
    "buffer","stream","window","offset","length","match","literal","distance",
    "encode","decode","compress","inflate","deflate","symbol","table","header",
    "record","field","value","index","parser","token","state","node"
};
#define NVOCAB ((int)(sizeof(VOCAB) / sizeof(VOCAB[0])))

/* Squaring a uniform draw front-loads it onto the common words. That is what
 * makes the deflate hash chains behave like they do on prose instead of on a
 * uniform word soup, and it gives the matcher a realistic hit rate. */
static int pick_word(void) {
    uint32_t r = crnd() >> 16;
    uint32_t sq = (r * r) >> 16;
    return (int)((sq * (uint32_t)NVOCAB) >> 16);
}

static size_t emit_word(char *b, size_t n, size_t cap) {
    const char *w = VOCAB[pick_word()];
    while (*w && n < cap) b[n++] = *w++;
    return n;
}

/* Prose: words separated by spaces, a newline every ~16 words.
 * Fills up to cap-24 bytes; needs cap > 32. */
static size_t make_text(char *buf, size_t cap, uint32_t seed) {
    size_t n = 0;
    corpus_seed(seed);
    while (n + 24 < cap) {
        n = emit_word(buf, n, cap);
        buf[n++] = ((crnd() >> 20) % 16u == 0u) ? '\n' : ' ';
    }
    buf[n ? n - 1 : 0] = '\n';
    return n;
}

/* Well-formed XML: nested elements, attributes, text content.
 *   maxdepth   deepest element nesting, 2..CORPUS_MAX_DEPTH
 *   attr_pct   chance (%) of one more attribute on an opening tag, 0..95
 * NUL-terminated; needs cap > 512. */
#define CORPUS_MAX_DEPTH 32
static size_t make_xml(char *buf, size_t cap, uint32_t seed, int maxdepth, int attr_pct) {
    const char *stack[CORPUS_MAX_DEPTH + 1];
    int depth = 0;
    size_t n = 0;
    corpus_seed(seed);

    buf[n++] = '<'; buf[n++] = 'r'; buf[n++] = '>';
    stack[depth++] = "r";

    while (n + 256 < cap) {
        uint32_t r = crnd() % 100u;
        if (depth < maxdepth && r < 45u) {                 /* open a child */
            const char *tag = VOCAB[pick_word()];
            buf[n++] = '<';
            for (const char *p = tag; *p; p++) buf[n++] = *p;
            while ((crnd() % 100u) < (uint32_t)attr_pct) {  /* attributes */
                const char *a = VOCAB[pick_word()];
                buf[n++] = ' ';
                for (const char *p = a; *p; p++) buf[n++] = *p;
                buf[n++] = '='; buf[n++] = '"';
                n = emit_word(buf, n, cap);
                buf[n++] = '"';
                if (n + 64 >= cap) break;
            }
            buf[n++] = '>';
            stack[depth++] = tag;
        } else if (r < 80u) {                              /* text content */
            int words = 1 + (int)(crnd() % 6u);
            for (int i = 0; i < words && n + 32 < cap; i++) {
                n = emit_word(buf, n, cap);
                buf[n++] = ' ';
            }
        } else if (depth > 1) {                            /* close */
            const char *tag = stack[--depth];
            buf[n++] = '<'; buf[n++] = '/';
            for (const char *p = tag; *p; p++) buf[n++] = *p;
            buf[n++] = '>';
        }
    }
    /* Closing tags are at most ~10 bytes each; the 256-byte margin is not
     * enough for 32 of them, so the caller's buffer carries extra slack. */
    while (depth > 1) {                                    /* close the rest */
        const char *tag = stack[--depth];
        buf[n++] = '<'; buf[n++] = '/';
        for (const char *p = tag; *p; p++) buf[n++] = *p;
        buf[n++] = '>';
    }
    buf[n++] = '<'; buf[n++] = '/'; buf[n++] = 'r'; buf[n++] = '>';
    buf[n] = '\0';
    return n;
}

/* Well-formed JSON: nested objects and arrays, strings, numbers, literals.
 *   maxdepth   deepest nesting, 1..CORPUS_MAX_DEPTH
 *
 * The close decision comes FIRST, before any separator or key is written. Doing
 * it the other way round emits a comma before a closing bracket and lets an
 * object key be followed by nothing, both of which cJSON rejects — and a parser
 * benchmark whose input fails to parse measures the error path.
 * NUL-terminated; needs cap > 512. */
static size_t make_json(char *buf, size_t cap, uint32_t seed, int maxdepth) {
    size_t n = 0;
    int depth = 0;
    char kind[CORPUS_MAX_DEPTH + 1];
    corpus_seed(seed);
    buf[n++] = '['; kind[depth++] = ']';

    while (n + 256 < cap) {
        if (depth > 1 && (crnd() % 100u) >= 88u) {     /* close, before anything else */
            buf[n++] = kind[--depth];
            continue;
        }
        if (buf[n - 1] != '[' && buf[n - 1] != '{') buf[n++] = ',';
        if (kind[depth - 1] == '}') {                  /* key, always followed by a value */
            buf[n++] = '"'; n = emit_word(buf, n, cap); buf[n++] = '"'; buf[n++] = ':';
        }
        uint32_t r = crnd() % 100u;
        if (depth < maxdepth && r < 30u) {             /* nest */
            char open = (crnd() & 1u) ? '{' : '[';
            buf[n++] = open;
            kind[depth++] = (open == '{') ? '}' : ']';
        } else if (r < 60u) {                          /* string */
            buf[n++] = '"'; n = emit_word(buf, n, cap); buf[n++] = '"';
        } else if (r < 85u) {                          /* number */
            uint32_t x = crnd() % 100000u;
            char tmp[12]; int k = 0;
            do { tmp[k++] = (char)('0' + x % 10u); x /= 10u; } while (x);
            while (k) buf[n++] = tmp[--k];
        } else {                                       /* literal */
            const char *lit = (r < 90u) ? "true" : (r < 95u ? "false" : "null");
            while (*lit) buf[n++] = *lit++;
        }
    }
    while (depth > 0) buf[n++] = kind[--depth];
    buf[n] = '\0';
    return n;
}

/* One 32-bit value depending on every byte consumed, printed by each workload so
 * a miscompile shows up as a wrong number rather than as a fast run. */
static uint32_t fold32(uint32_t h, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h * 16777619u) ^ p[i];
    return h;
}
#endif

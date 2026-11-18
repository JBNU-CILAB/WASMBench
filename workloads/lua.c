/* Lua 5.5 interpreter running a word-count / pattern-matching script.
 *
 * Two hot populations:
 *
 *   the VM dispatch in lvm.c   every opcode handler opens with a chain of type
 *                              tests — ttisinteger, then ttisfloat, then the
 *                              metamethod fallback — which is a short-circuit
 *                              chain feeding one join, and the fast arm wins
 *                              almost every time.
 *   the pattern matcher        lstrlib.c carries a backtracking matcher of its
 *                              own, reached through string.find and gsub.
 *
 * -Dluai_makeseed()=0u pins Lua's string-hash seed. Left alone it is derived
 * from a stack address and the clock, which makes table iteration order — and
 * therefore the branch trace — differ between runs of the same binary on the
 * same input. That is not noise the measurement can average away; it changes
 * what is executed.
 *
 * Knobs (-D):
 *   ITERS  40        script runs over the input
 *   BYTES  48*1024   generated input text size
 *   SORT   1         0 skips the table.sort phase
 *   SEED   0x9E3779B9 input RNG seed (CORPUS_SEED_TRAIN for the train input)
 */
#define BENCH_NAME "lua"
#include "bench.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifndef ITERS
#define ITERS 40
#endif
#ifndef BYTES
#define BYTES (48 * 1024)
#endif
#ifndef SORT
#define SORT 1
#endif

static const char *SCRIPT =
"local text = INPUT\n"
"local counts, order = {}, {}\n"
"for line in string.gmatch(text, '[^\\n]+') do\n"
"  for w in string.gmatch(line, '%a+') do\n"
"    local c = counts[w]\n"
"    if c == nil then counts[w] = 1; order[#order+1] = w\n"
"    else counts[w] = c + 1 end\n"
"  end\n"
"  if string.find(line, '[a-z]+er') then HITS = HITS + 1 end\n"
"  if string.find(line, '^the') then HITS = HITS + 2 end\n"
"  local s = string.gsub(line, '(%a)(%a*)', '%2%1')\n"
"  ACC = (ACC + #s) % 4294967296\n"
"end\n"
"if SORT then\ntable.sort(order, function(a, b)\n"
"  if counts[a] ~= counts[b] then return counts[a] > counts[b] end\n"
"  return a < b\n"
"end)\nend\n"
"for i = 1, #order do ACC = (ACC * 31 + counts[order[i]] + #order[i]) % 4294967296 end\n";

BENCH_EXPORT uint32_t runtime(void) {
    char *text = bench_alloc(BYTES);
    size_t n = make_text(text, BYTES, SEED);

    lua_State *L = luaL_newstate();
    if (!L) bench_fail("luaL_newstate");
    luaL_openlibs(L);
    lua_pushlstring(L, text, n);
    lua_setglobal(L, "INPUT");
    lua_pushboolean(L, SORT);
    lua_setglobal(L, "SORT");

    double acc = 0, hits = 0;
    for (long it = 0; it < ITERS; it++) {
        lua_pushnumber(L, 0); lua_setglobal(L, "ACC");
        lua_pushnumber(L, 0); lua_setglobal(L, "HITS");
        if (luaL_dostring(L, SCRIPT) != LUA_OK) bench_fail("%s", lua_tostring(L, -1));
        lua_getglobal(L, "ACC");  acc  += lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getglobal(L, "HITS"); hits += lua_tonumber(L, -1); lua_pop(L, 1);
    }
    lua_close(L);
    free(text);
    /* Both accumulators are exact integers below 2^53. */
    uint32_t h = 2166136261u;
    uint64_t a = (uint64_t)acc, t = (uint64_t)hits;
    h = fold32(h, (const unsigned char *)&a, sizeof a);
    h = fold32(h, (const unsigned char *)&t, sizeof t);
    return h;
}

BENCH_MAIN

/* loslib.c references a handful of hosted-C facilities wasi-libc does not
 * provide. The script calls none of them — no os.clock, no os.execute, no
 * os.tmpname — so they are stubbed rather than emulated. Each stub fails
 * visibly if something ever does reach it, instead of returning a plausible
 * value that would quietly enter a measurement.
 *
 * clock() in particular must not become a real timer: a workload that reads the
 * clock is no longer reproducible to the instruction. */
#include <stdio.h>
#include <time.h>
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { (void)s; return NULL; }
int system(const char *c) { (void)c; return -1; }
clock_t clock(void) { return (clock_t)0; }

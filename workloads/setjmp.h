/* Shim intercepting <setjmp.h> for the Lua workload.
 *
 * wasm32-wasip1's own setjmp.h is a hard #error: setjmp/longjmp on wasm needs
 * the exception-handling proposal, and wasi-sdk 34 ships no runtime for it in
 * this sysroot. Enabling EH instead would put the engine's exception support
 * inside the measurement, where it could interact with the very JIT pass being
 * measured — so the dependency is removed rather than satisfied.
 *
 * Lua never reaches these: the build also defines LUAI_THROW to abort() and
 * LUAI_TRY to a plain call, so the interpreter runs unprotected. A script error
 * therefore aborts loudly instead of unwinding, which is the right failure mode
 * for a benchmark whose script must not error at all.
 *
 * -I workloads precedes the sysroot, so this wins for both quoted and angle
 * includes.
 */
#ifndef WALRUSBENCH_SETJMP_SHIM_H
#define WALRUSBENCH_SETJMP_SHIM_H
#include <stdlib.h>
typedef int jmp_buf[1];
typedef int sigjmp_buf[1];
#define setjmp(b)          (0)
#define _setjmp(b)         (0)
#define sigsetjmp(b, s)    (0)
#define longjmp(b, v)      abort()
#define _longjmp(b, v)     abort()
#define siglongjmp(b, v)   abort()
#endif

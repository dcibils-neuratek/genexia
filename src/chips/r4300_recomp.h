/*
 * r4300_recomp.h - what recompiled MIPS blocks see of the R4300i.
 *
 * tools/recompile_mips.py turns game code into C functions, one per basic
 * block. A block does exactly what the interpreter would do, cycle for cycle:
 * it refuses to start unless every instruction in it would begin before
 * s_limit, and anything off the fast path (registers, mapped memory, faults,
 * FPU traps) leaves the block at that instruction for the interpreter. So
 * recompiled and interpreted runs are identical, which is how this is tested.
 *
 * A block takes `used` and returns it advanced, r4_pc holding where to go on.
 * RDRAM may be overwritten by overlays, so a block is only installed in
 * r4_active once RDRAM is seen to hold the words it was generated from.
 */
#ifndef R4300_RECOMP_H
#define R4300_RECOMP_H
#include "n64.h"
#include <stdio.h>
#include <string.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

typedef int (*R4Block)(int used);

enum { R4_EXIT_DELAY = 1, R4_EXIT_STEP = 2, R4_EXIT_STALL = 4 };

extern R4Block *r4_active;         /* indexed by (pc & 0x7FFFFF) >> 2 */
extern u32 r4_pc, r4_npc, r4_exit;
extern u64 s_hi, s_lo;
extern u32 s_fpw[64], s_fcr31;
extern int s_limit;
int r4300_cop1(u32 inst);          /* the interpreter's COP1 ops: extra cycles or -1 */

void r4300_recomp_add(u32 pc, R4Block fn, const u32 *words, u32 n);
void r4300_recomp_enable(void);    /* blocks may run */
void r4300_recomp_flush(void);     /* RDRAM changed wholesale */
void r4300_code_written(u32 pa, u32 n);
void r4300_recomp_log(FILE *f, const u32 *rom_of);
void r4300_recomp_print(void);

#define FC_CLEAR (s_fcr31 &= ~0x3F000u)

static inline u16 r4_rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
static inline u32 r4_rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline u64 r4_rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline void r4_wr16(u8 *p, u16 x) { x = __builtin_bswap16(x); memcpy(p, &x, 2); }
static inline void r4_wr32(u8 *p, u32 x) { x = __builtin_bswap32(x); memcpy(p, &x, 4); }
static inline void r4_wr64(u8 *p, u64 x) { x = __builtin_bswap64(x); memcpy(p, &x, 8); }

static inline u64 r4_fgl(int i) { return (u64)s_fpw[i + 1] << 32 | s_fpw[i]; }
static inline void r4_fsl(int i, u64 v) { s_fpw[i] = (u32)v; s_fpw[i + 1] = (u32)(v >> 32); }
static inline float r4_fgs(int i) { float f; memcpy(&f, &s_fpw[i], 4); return f; }
static inline void r4_fss(int i, float f) { memcpy(&s_fpw[i], &f, 4); }
static inline double r4_fgd(int i) { u64 w = r4_fgl(i); double d; memcpy(&d, &w, 8); return d; }
static inline void r4_fsd(int i, double d) { u64 w; memcpy(&w, &d, 8); r4_fsl(i, w); }

/* results the interpreter must classify: NaN, infinity, denormal (not zero) */
static inline int r4_odd_s(float r) {
    u32 u; memcpy(&u, &r, 4);
    u32 e = (u >> 23) & 0xFF;
    return e == 0xFF || (e == 0 && (u & 0x7FFFFFu));
}
static inline int r4_odd_d(double r) {
    u64 u; memcpy(&u, &r, 8);
    u32 e = (u32)(u >> 52) & 0x7FF;
    return e == 0x7FF || (e == 0 && (u & 0xFFFFFFFFFFFFFull));
}

#define SX(x) ((u64)(s64)(s32)(x))
#define BAIL(p, n, d, k) do { r4_pc = (p); r4_npc = (n); r4_exit = R4_EXIT_STEP | (d); return used + (k); } while (0)
#define STALL(p) do { r4_pc = (p); r4_exit = R4_EXIT_STALL; return used; } while (0)
#define EXIT(t, k) do {                                                       \
        R4Block n_ = r4_active[((t) & 0x7FFFFFu) >> 2];                       \
        used += (k);                                                          \
        if (likely(n_)) __attribute__((musttail)) return n_(used);            \
        r4_pc = (t);                                                          \
        return used;                                                          \
    } while (0)
/* jump to a computed address */
#define EXITR(t, k) do {                                                      \
        u32 x_ = (t);                                                         \
        if (likely((u32)(x_ - 0x80000000u) < N64_RDRAM_SIZE && !(x_ & 3))) EXIT(x_, k); \
        r4_pc = x_;                                                           \
        return used + (k);                                                    \
    } while (0)

static inline void r4_mult(s32 a, s32 b) { s64 r = (s64)a * b; s_lo = SX((u32)r); s_hi = SX((u32)(r >> 32)); }
static inline void r4_multu(u32 a, u32 b) { u64 r = (u64)a * b; s_lo = SX((u32)r); s_hi = SX((u32)(r >> 32)); }
static inline void r4_div(s32 n, s32 d) {
    if (d == 0) { s_lo = n < 0 ? 1 : ~0ull; s_hi = SX(n); }
    else if (d == -1) { s_lo = SX(0u - (u32)n); s_hi = 0; }
    else { s_lo = SX(n / d); s_hi = SX(n % d); }
}
static inline void r4_divu(u32 n, u32 d) {
    if (d == 0) { s_lo = ~0ull; s_hi = SX(n); }
    else { s_lo = SX(n / d); s_hi = SX(n % d); }
}
static inline void r4_dmult(s64 a, s64 b) { __int128 r = (__int128)a * b; s_lo = (u64)r; s_hi = (u64)(r >> 64); }
static inline void r4_dmultu(u64 a, u64 b) { unsigned __int128 r = (unsigned __int128)a * b; s_lo = (u64)r; s_hi = (u64)(r >> 64); }
static inline void r4_ddiv(s64 n, s64 d) {
    if (d == 0) { s_lo = n < 0 ? 1 : ~0ull; s_hi = (u64)n; }
    else if (d == -1) { s_lo = 0 - (u64)n; s_hi = 0; }
    else { s_lo = (u64)(n / d); s_hi = (u64)(n % d); }
}
static inline void r4_ddivu(u64 n, u64 d) {
    if (d == 0) { s_lo = ~0ull; s_hi = n; }
    else { s_lo = n / d; s_hi = n % d; }
}
#endif

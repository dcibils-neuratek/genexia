/*
 * r4300_compact.h - what blocks from tools/recompile_mips32.py see of the R4300i.
 *
 * Unlike r4300_recomp.h, these blocks don't reproduce the interpreter cycle by
 * cycle. Registers live in locals and go back to r4300_gpr at the end of a block,
 * always sign-extended, because the 64-bit instructions that could break that
 * run on the interpreter. Interrupts are seen between blocks. `used` is an
 * estimate, which only moves the timers.
 *
 * r4300_gpr and s_hi/s_lo are 64-bit in memory; a block only touches the half it
 * needs, so all of this works in either byte order.
 */
#ifndef R4300_COMPACT_H
#define R4300_COMPACT_H
#include "r4300_recomp.h"
#include <math.h>

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define L(n) ((n) * 2 + 1)
#define H(n) ((n) * 2)
#define r4_rw32(p) (*(const u32 *)(p))
#define r4_rw16(p) (*(const u16 *)(p))
#define r4_ww32(p, v) (*(u32 *)(p) = (v))
#define r4_ww16(p, v) (*(u16 *)(p) = (v))
#else
#define L(n) ((n) * 2)
#define H(n) ((n) * 2 + 1)
#define r4_rw32 r4_rd32
#define r4_rw16 r4_rd16
#define r4_ww32 r4_wr32
#define r4_ww16 r4_wr16
#endif
#define r4_low_words ((u32 *)r4300_gpr)
#define SET(n, v) do { g[L(n)] = (v); g[H(n)] = (u32)((s32)(v) >> 31); } while (0)

typedef struct { u32 pc; R4Block fn; const u32 *words; u32 n; } R4Compact;
void r4300_compact_add(const R4Compact *t, unsigned n);

u32  r4_io_read(u32 a, int size);
void r4_io_write(u32 a, u32 v, int size);

static inline u32 r4_low(const u64 *p) { return (u32)*p; }
static inline u64 r4_get64(int n) { return r4300_gpr[n]; }
static inline void r4_gpr64(int n, u64 v) { r4300_gpr[n] = v; }

#ifdef R4_LOCKSTEP
void r4_log(u32 a, int n);         /* make n64c-lockstep: a block is about to write here */
#else
#define r4_log(a, n) ((void)0)
#endif
#define RAM(a) ((u32)((a) - 0x80000000u) < N64_RDRAM_SIZE)
#define AT(a) (n64_rdram + ((a) - 0x80000000u))
static inline u32 r4_lw(u32 a) { return likely(RAM(a)) ? r4_rw32(AT(a)) : r4_io_read(a, 4); }
static inline u32 r4_lh(u32 a) { return (u32)(s32)(s16)(likely(RAM(a)) ? r4_rw16(AT(a)) : r4_io_read(a, 2)); }
static inline u32 r4_lhu(u32 a) { return likely(RAM(a)) ? r4_rw16(AT(a)) : r4_io_read(a, 2) & 0xFFFF; }
static inline u32 r4_lb(u32 a) { return (u32)(s32)(s8)(likely(RAM(a)) ? *AT(a) : r4_io_read(a, 1)); }
static inline u32 r4_lbu(u32 a) { return likely(RAM(a)) ? *AT(a) : r4_io_read(a, 1) & 0xFF; }
static inline void r4_sw(u32 a, u32 v) { if (likely(RAM(a))) { r4_log(a, 4); r4_ww32(AT(a), v); } else r4_io_write(a, v, 4); }
static inline void r4_sh(u32 a, u32 v) { if (likely(RAM(a))) { r4_log(a, 2); r4_ww16(AT(a), (u16)v); } else r4_io_write(a, v, 2); }
static inline void r4_sb(u32 a, u32 v) { if (likely(RAM(a))) { r4_log(a, 1); *AT(a) = (u8)v; } else r4_io_write(a, v, 1); }
static inline u64 r4_ld(u32 a) { const u32 h = r4_lw(a); return (u64)h << 32 | r4_lw(a + 4); }
static inline void r4_sd(u32 a, u64 v) { r4_sw(a, (u32)(v >> 32)); r4_sw(a + 4, (u32)v); }
static inline u32 r4_lwl(u32 r, u32 a) {
    const u32 sh = (a & 3) * 8, m = r4_lw(a & ~3u);
    return (r & ((1ull << sh) - 1)) | (m << sh);
}
static inline u32 r4_lwr(u32 r, u32 a) {
    const u32 sh = (3 - (a & 3)) * 8, m = r4_lw(a & ~3u);
    return (r & ~(u32)(0xFFFFFFFFull >> sh)) | (m >> sh);
}
static inline void r4_swl(u32 a, u32 v) {
    const u32 sh = (a & 3) * 8, mk = 0xFFFFFFFFu >> sh, q = a & ~3u;
    r4_sw(q, (r4_lw(q) & ~mk) | (v >> sh));
}
static inline void r4_swr(u32 a, u32 v) {
    const u32 sh = (3 - (a & 3)) * 8, q = a & ~3u;
    r4_sw(q, (r4_lw(q) & (u32)((1ull << sh) - 1)) | (v << sh));
}
#ifdef GENEXIA_OS9
float r4_sqrtf(float x);
double r4_sqrt(double x);
#else
#define r4_sqrtf sqrtf
#define r4_sqrt sqrt
#endif

/* leave for pc: straight to the block when it is in this file and time is left */
#define GO(f, pc, k) do {                                                     \
        used += (k);                                                          \
        if (f && likely(used < s_limit)) return ((R4Block)(f))(used);         \
        r4_pc = (pc);                                                         \
        return used;                                                          \
    } while (0)
/* the interpreter takes the instruction at pc */
#define STEP(pc, k) do { r4_pc = (pc); r4_exit = R4_EXIT_STEP; return used + (k); } while (0)
/* idle loop: nothing can change until the next event */
#define IDLE(pc) do { r4_pc = (pc); return used < s_limit ? s_limit : used + 2; } while (0)

static inline int r4_jump(u32 t, int used) {
    r4_pc = t;
    if (likely((u32)(t - 0x80000000u) < N64_RDRAM_SIZE && !(t & 3) && used < s_limit)) {
        R4Block n = r4_active[(t & 0x7FFFFFu) >> 2];
        if (likely(n)) return n(used);
    }
    return used;
}
#endif

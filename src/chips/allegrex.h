/*
 * allegrex.h - what recompiled PSP code sees of the Allegrex CPU.
 *
 * tools/recompile_psp.py turns a game into C functions, one per block. A block
 * returns the pc to go on at. All registers live in `p`, so switching PSP threads
 * is a matter of swapping that struct between blocks. Guest memory is one host
 * mapping of the 512 MB address space, so a load or store is a plain access.
 */
#ifndef ALLEGREX_H
#define ALLEGREX_H
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

typedef union { float s; u32 w; s32 i; } PspFloat;
typedef struct {
    u32 r[32], hi, lo, pc;
    PspFloat f[32];
    u32 fcr31;
    u32 vcc;                  /* VFPU condition bits */
    float v[128];             /* VFPU registers */
    u32 vctl[16];
} PspCpu;
extern PspCpu p;
extern u8 *psp_mem;
typedef u32 (*PspBlock)(void);

void psp_block_add(u32 pc, PspBlock fn);
void psp_import_add(u32 pc, const char *module, u32 nid);
u32  psp_trap(u32 pc, u32 inst);      /* break, or anything the recompiler couldn't place */
void psp_vfpu(u32 inst);              /* one VFPU instruction */
void psp_vfpu_load(u32 inst);         /* VFPU loads and stores */

#define M(a) (psp_mem + ((a) & 0x1FFFFFFFu))
static inline u8  RD8(u32 a)  { return *M(a); }
static inline u16 RD16(u32 a) { u16 v; memcpy(&v, M(a), 2); return v; }
static inline u32 RD32(u32 a) { u32 v; memcpy(&v, M(a), 4); return v; }
static inline void WR8(u32 a, u8 v)   { *M(a) = v; }
static inline void WR16(u32 a, u16 v) { memcpy(M(a), &v, 2); }
static inline void WR32(u32 a, u32 v) { memcpy(M(a), &v, 4); }

static inline u32 psp_lwl(u32 rt, u32 a) { u32 s = (a & 3) * 8; return (rt & (0x00FFFFFFu >> s)) | (RD32(a & ~3u) << (24 - s)); }
static inline u32 psp_lwr(u32 rt, u32 a) { u32 s = (a & 3) * 8; return (rt & (s ? 0xFFFFFFFFu << (32 - s) : 0)) | (RD32(a & ~3u) >> s); }
static inline void psp_swl(u32 rt, u32 a) { u32 s = (a & 3) * 8; WR32(a & ~3u, (RD32(a & ~3u) & (0xFFFFFF00u << s)) | (rt >> (24 - s))); }
static inline void psp_swr(u32 rt, u32 a) { u32 s = (a & 3) * 8; WR32(a & ~3u, (RD32(a & ~3u) & (s ? 0xFFFFFFFFu >> (32 - s) : 0)) | (rt << s)); }
static inline void psp_acc(s64 add) { u64 v = ((u64)p.hi << 32 | p.lo) + (u64)add; p.hi = (u32)(v >> 32); p.lo = (u32)v; }
static inline u32 psp_clz(u32 v) { return v ? (u32)__builtin_clz(v) : 32; }
static inline u32 psp_bitrev(u32 v) { u32 r = 0; for (int i = 0; i < 32; i++) r |= ((v >> i) & 1) << (31 - i); return r; }
static inline u32 psp_ror(u32 v, u32 n) { n &= 31; return n ? v >> n | v << (32 - n) : v; }
static inline void psp_div(s32 a, s32 b) {
    if (b == 0) { p.lo = a < 0 ? 1u : 0xFFFFFFFFu; p.hi = (u32)a; }
    else if (a == INT32_MIN && b == -1) { p.lo = (u32)a; p.hi = 0; }
    else { p.lo = (u32)(a / b); p.hi = (u32)(a % b); }
}
static inline void psp_divu(u32 a, u32 b) {
    if (b == 0) { p.lo = 0xFFFFFFFFu; p.hi = a; } else { p.lo = a / b; p.hi = a % b; }
}
static inline s32 psp_f2i(float v) {
    if (v != v || v >= 2147483648.0f || v <= -2147483649.0f) return 0x7FFFFFFF;
    return (s32)v;
}
#endif

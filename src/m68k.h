/*
 * m68k.h - Motorola 68000 CPU state, memory access and instruction
 * semantics shared by the generated interpreter (m68k_interp.c) and the
 * statically recompiled game code (generated/recomp_*.c).
 *
 * Everything that defines *what an instruction does* lives here as small
 * inline helpers so that interpreter and recompiler cannot diverge.
 */
#ifndef M68K_H
#define M68K_H
#include "genesis.h"

typedef struct {
    u32 d[8];
    u32 a[8];          /* a[7] is the active stack pointer */
    u32 pc;            /* next instruction (interp: advanced by fetch) */
    u32 ppc;           /* address of the instruction being executed (interp) */
    u32 usp, ssp;      /* inactive stack pointer is parked here */
    u32 ir;            /* current opcode (interp) */
    u8  c, v, z, n, x; /* condition codes, each 0/1 */
    u8  s, t;          /* supervisor, trace */
    u8  imask;         /* interrupt mask 0..7 */
    u8  stopped;       /* STOP executed, waiting for interrupt */
    s64 cyc;           /* 68k cycles executed (monotonic) */
    int irq_level;     /* current interrupt request level from the VDP */
} M68K;

extern M68K cpu;

/* ------------------------------------------------------------ memory */
extern u8  rom[0x400000];
extern u8  ram[0x10000];
extern u32 rom_size;

u8   io_read8(u32 a);
u16  io_read16(u32 a);
void io_write8(u32 a, u8 v);
void io_write16(u32 a, u16 v);

static inline u32 rd8(u32 a) {
    a &= 0xFFFFFF;
    if (a < 0x400000) return rom[a];
    if (a >= 0xE00000) return ram[a & 0xFFFF];
    return io_read8(a);
}
static inline u32 rd16(u32 a) {
    a &= 0xFFFFFE;
    if (a < 0x400000) return (u32)(rom[a] << 8 | rom[a + 1]);
    if (a >= 0xE00000) { a &= 0xFFFF; return (u32)(ram[a] << 8 | ram[a + 1]); }
    return io_read16(a);
}
static inline u32 rd32(u32 a) { return rd16(a) << 16 | rd16(a + 2); }

static inline void wr8(u32 a, u32 v) {
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { ram[a & 0xFFFF] = (u8)v; return; }
    if (a >= 0x400000) io_write8(a, (u8)v);
}
static inline void wr16(u32 a, u32 v) {
    a &= 0xFFFFFE;
    if (a >= 0xE00000) { a &= 0xFFFF; ram[a] = (u8)(v >> 8); ram[a + 1] = (u8)v; return; }
    if (a >= 0x400000) io_write16(a, (u16)v);
}
static inline void wr32(u32 a, u32 v) { wr16(a, v >> 16); wr16(a + 2, v); }

static inline u32 fetch16(void) { u32 w = rd16(cpu.pc); cpu.pc += 2; return w; }

/* ---------------------------------------------------------- registers */
#define SETD8(r, v)  (cpu.d[r] = (cpu.d[r] & 0xFFFFFF00u) | ((u32)(v) & 0xFFu))
#define SETD16(r, v) (cpu.d[r] = (cpu.d[r] & 0xFFFF0000u) | ((u32)(v) & 0xFFFFu))
#define SETD32(r, v) (cpu.d[r] = (u32)(v))
#define BINC(r)      ((r) == 7 ? 2u : 1u)   /* byte (An)+/-(An) step, A7 keeps word alignment */
#define SX8(v)  ((u32)(s32)(s8)(v))
#define SX16(v) ((u32)(s32)(s16)(v))

/* brief extension word index (d8,An,Xn) on 68000: disp8 + Xn.W/L */
static inline u32 m68k_index(u32 w) {
    u32 r = (w >> 12) & 7;
    u32 x = (w & 0x8000) ? cpu.a[r] : cpu.d[r];
    if (!(w & 0x0800)) x = SX16(x);
    return x + SX8(w);
}

static inline void push16(u32 v) { cpu.a[7] -= 2; wr16(cpu.a[7], v); }
static inline void push32(u32 v) { cpu.a[7] -= 4; wr32(cpu.a[7], v); }
static inline u32 pop16(void) { u32 v = rd16(cpu.a[7]); cpu.a[7] += 2; return v; }
static inline u32 pop32(void) { u32 v = rd32(cpu.a[7]); cpu.a[7] += 4; return v; }

/* ------------------------------------------------------------- status */
static inline u32 get_ccr(void) {
    return (u32)(cpu.x << 4 | cpu.n << 3 | cpu.z << 2 | cpu.v << 1 | cpu.c);
}
static inline u32 get_sr(void) {
    return (u32)(cpu.t << 15 | cpu.s << 13 | cpu.imask << 8) | get_ccr();
}
static inline void set_ccr(u32 v) {
    cpu.x = (v >> 4) & 1; cpu.n = (v >> 3) & 1; cpu.z = (v >> 2) & 1;
    cpu.v = (v >> 1) & 1; cpu.c = v & 1;
}
static inline void set_sr(u32 v) {
    u8 ns = (v >> 13) & 1;
    if (ns != cpu.s) {
        if (ns) { cpu.usp = cpu.a[7]; cpu.a[7] = cpu.ssp; }
        else    { cpu.ssp = cpu.a[7]; cpu.a[7] = cpu.usp; }
        cpu.s = ns;
    }
    cpu.t = (v >> 15) & 1;
    cpu.imask = (v >> 8) & 7;
    set_ccr(v);
}

/* Exception processing: pushes `ret` and SR, enters supervisor mode and
 * loads the vector. cpu.pc is set to the handler. */
void m68k_exception(int vec, u32 ret);
void m68k_reset(void);

static inline int m68k_cond(u32 cc) {
    switch (cc & 15) {
    case 0: return 1;                case 1: return 0;
    case 2: return !cpu.c && !cpu.z; case 3: return cpu.c || cpu.z;
    case 4: return !cpu.c;           case 5: return cpu.c;
    case 6: return !cpu.z;           case 7: return cpu.z;
    case 8: return !cpu.v;           case 9: return cpu.v;
    case 10: return !cpu.n;          case 11: return cpu.n;
    case 12: return cpu.n == cpu.v;  case 13: return cpu.n != cpu.v;
    case 14: return !cpu.z && cpu.n == cpu.v;
    default: return cpu.z || cpu.n != cpu.v;
    }
}

/* --------------------------------------------------------------- ALU */
#define SZMASK(sz) ((sz) == 1 ? 0xFFu : (sz) == 2 ? 0xFFFFu : 0xFFFFFFFFu)
#define SZMSB(sz)  ((sz) == 1 ? 0x80u : (sz) == 2 ? 0x8000u : 0x80000000u)

static inline void f_nz(int sz, u32 r) {
    r &= SZMASK(sz);
    cpu.n = (r & SZMSB(sz)) != 0; cpu.z = r == 0;
}
static inline u32 f_logic(int sz, u32 r) {
    r &= SZMASK(sz); f_nz(sz, r); cpu.v = 0; cpu.c = 0; return r;
}
static inline u32 f_add(int sz, u32 s, u32 d) {
    u32 m = SZMSB(sz), r = (d + s) & SZMASK(sz);
    cpu.n = (r & m) != 0; cpu.z = r == 0;
    cpu.v = (((s ^ r) & (d ^ r)) & m) != 0;
    cpu.c = cpu.x = (((s & d) | (~r & (s | d))) & m) != 0;
    return r;
}
static inline u32 f_addx(int sz, u32 s, u32 d) {
    u32 m = SZMSB(sz), r = (d + s + cpu.x) & SZMASK(sz);
    cpu.n = (r & m) != 0; if (r) cpu.z = 0;
    cpu.v = (((s ^ r) & (d ^ r)) & m) != 0;
    cpu.c = cpu.x = (((s & d) | (~r & (s | d))) & m) != 0;
    return r;
}
static inline u32 f_sub(int sz, u32 s, u32 d) {
    u32 m = SZMSB(sz), r = (d - s) & SZMASK(sz);
    cpu.n = (r & m) != 0; cpu.z = r == 0;
    cpu.v = (((s ^ d) & (r ^ d)) & m) != 0;
    cpu.c = cpu.x = (((s & r) | (~d & (s | r))) & m) != 0;
    return r;
}
static inline u32 f_subx(int sz, u32 s, u32 d) {
    u32 m = SZMSB(sz), r = (d - s - cpu.x) & SZMASK(sz);
    cpu.n = (r & m) != 0; if (r) cpu.z = 0;
    cpu.v = (((s ^ d) & (r ^ d)) & m) != 0;
    cpu.c = cpu.x = (((s & r) | (~d & (s | r))) & m) != 0;
    return r;
}
static inline void f_cmp(int sz, u32 s, u32 d) {
    u32 m = SZMSB(sz), r = (d - s) & SZMASK(sz);
    cpu.n = (r & m) != 0; cpu.z = r == 0;
    cpu.v = (((s ^ d) & (r ^ d)) & m) != 0;
    cpu.c = (((s & r) | (~d & (s | r))) & m) != 0;
}

/* shifts / rotates; cnt is the already-reduced count (0..63) */
static inline u32 f_asl(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0;
    if (!cnt) { cpu.c = 0; f_nz(sz, v); return v; }
    for (u32 i = 0; i < cnt; i++) {
        u32 nv = (v << 1) & mask;
        cpu.c = (v & m) != 0;
        if ((nv ^ v) & m) cpu.v = 1;
        v = nv;
    }
    cpu.x = cpu.c; f_nz(sz, v); return v;
}
static inline u32 f_asr(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0;
    if (!cnt) { cpu.c = 0; f_nz(sz, v); return v; }
    for (u32 i = 0; i < cnt; i++) { cpu.c = v & 1; v = (v >> 1) | (v & m); }
    cpu.x = cpu.c; f_nz(sz, v); return v;
}
static inline u32 f_lsl(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0;
    if (!cnt) { cpu.c = 0; f_nz(sz, v); return v; }
    for (u32 i = 0; i < cnt; i++) { cpu.c = (v & m) != 0; v = (v << 1) & mask; }
    cpu.x = cpu.c; f_nz(sz, v); return v;
}
static inline u32 f_lsr(int sz, u32 v, u32 cnt) {
    u32 mask = SZMASK(sz);
    v &= mask; cpu.v = 0;
    if (!cnt) { cpu.c = 0; f_nz(sz, v); return v; }
    for (u32 i = 0; i < cnt; i++) { cpu.c = v & 1; v >>= 1; }
    cpu.x = cpu.c; f_nz(sz, v); return v;
}
static inline u32 f_rol(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0; cpu.c = 0;
    for (u32 i = 0; i < cnt; i++) { cpu.c = (v & m) != 0; v = ((v << 1) | cpu.c) & mask; }
    f_nz(sz, v); return v;
}
static inline u32 f_ror(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0; cpu.c = 0;
    for (u32 i = 0; i < cnt; i++) { cpu.c = v & 1; v = (v >> 1) | (cpu.c ? m : 0); }
    f_nz(sz, v); return v;
}
static inline u32 f_roxl(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0;
    for (u32 i = 0; i < cnt; i++) { u32 o = (v & m) != 0; v = ((v << 1) | cpu.x) & mask; cpu.x = (u8)o; }
    cpu.c = cpu.x; f_nz(sz, v); return v;
}
static inline u32 f_roxr(int sz, u32 v, u32 cnt) {
    u32 m = SZMSB(sz), mask = SZMASK(sz);
    v &= mask; cpu.v = 0;
    for (u32 i = 0; i < cnt; i++) { u32 o = v & 1; v = (v >> 1) | (cpu.x ? m : 0); cpu.x = (u8)o; }
    cpu.c = cpu.x; f_nz(sz, v); return v;
}

/* BCD */
static inline u32 f_abcd(u32 s, u32 d) {
    u32 r = (s & 0x0F) + (d & 0x0F) + cpu.x;
    if (r > 9) r += 6;
    r += (s & 0xF0) + (d & 0xF0);
    cpu.c = cpu.x = r > 0x99;
    if (cpu.c) r -= 0xA0;
    r &= 0xFF; cpu.n = (r & 0x80) != 0; if (r) cpu.z = 0; cpu.v = 0;
    return r;
}
static inline u32 f_sbcd(u32 s, u32 d) {
    u32 r = (d & 0x0F) - (s & 0x0F) - cpu.x;
    if (r > 9) r -= 6;
    r += (d & 0xF0) - (s & 0xF0);
    cpu.c = cpu.x = r > 0x99;
    if (cpu.c) r += 0xA0;
    r &= 0xFF; cpu.n = (r & 0x80) != 0; if (r) cpu.z = 0; cpu.v = 0;
    return r;
}

/* multiply / divide. Division returns 0 and raises the exception itself
 * when dividing by zero (caller must stop executing the block). */
static inline u32 f_mulu(u32 s, u32 d) { u32 r = (s & 0xFFFF) * (d & 0xFFFF); f_logic(4, r); return r; }
static inline u32 f_muls(u32 s, u32 d) { u32 r = (u32)((s32)(s16)s * (s32)(s16)d); f_logic(4, r); return r; }
static inline u32 f_divu(u32 s, u32 d) {
    s &= 0xFFFF;
    u32 q = d / s, r = d % s;
    cpu.c = 0;
    if (q > 0xFFFF) { cpu.v = 1; cpu.n = 1; return d; }
    cpu.v = 0; cpu.n = (q & 0x8000) != 0; cpu.z = q == 0;
    return (r << 16) | q;
}
static inline u32 f_divs(u32 s, u32 d) {
    s32 sv = (s16)s, dv = (s32)d;
    cpu.c = 0;
    if (dv == (s32)0x80000000 && sv == -1) { cpu.v = 0; cpu.n = 0; cpu.z = 1; return 0; }
    s32 q = dv / sv, r = dv % sv;
    if (q < -32768 || q > 32767) { cpu.v = 1; cpu.n = 1; return d; }
    cpu.v = 0; cpu.n = (q & 0x8000) != 0; cpu.z = (q & 0xFFFF) == 0;
    return ((u32)(r & 0xFFFF) << 16) | ((u32)q & 0xFFFF);
}

/* MOVEM (interpreter path; the recompiler unrolls these) */
static inline u32 movem_store(u32 mask, u32 addr, int sz) {
    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
        u32 v = i < 8 ? cpu.d[i] : cpu.a[i - 8];
        if (sz == 2) wr16(addr, v); else wr32(addr, v);
        addr += (u32)sz;
    }
    return addr;
}
/* predecrement form: mask bit0 = A7 ... bit15 = D0, stores downwards */
static inline u32 movem_store_predec(u32 mask, u32 addr, int sz) {
    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
        int r = 15 - i;
        u32 v = r < 8 ? cpu.d[r] : cpu.a[r - 8];
        addr -= (u32)sz;
        if (sz == 2) wr16(addr, v); else wr32(addr, v);
    }
    return addr;
}
static inline u32 movem_load(u32 mask, u32 addr, int sz) {
    for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
        u32 v = sz == 2 ? SX16(rd16(addr)) : rd32(addr);
        if (i < 8) cpu.d[i] = v; else cpu.a[i - 8] = v;
        addr += (u32)sz;
    }
    return addr;
}
static inline int popcount16(u32 m) { return __builtin_popcount(m & 0xFFFF); }

/* ----------------------------------------------------------- dispatch */
typedef void (*blockfn)(void);
extern void (*const m68k_optable[65536])(void);
void m68k_interp_step(void);           /* execute one instruction at cpu.pc */
void m68k_run(s64 until_cycle);        /* dispatcher: recompiled blocks, interp fallback */
void recomp_register(void);            /* generated: fills the block table */
void recomp_add(u32 pc, blockfn f);
extern void (*m68k_register_game)(void);  /* set before reset; NULL = interpreter only */
void recomp_register_umk3(void);
extern int m68k_force_interp;          /* 1 = never use recompiled blocks (reference mode) */

#endif

/*
 * w65816_ops.h - 65C816 instruction semantics, shared by the interpreter
 * (w65816.c) and statically recompiled code, so the two cannot diverge.
 */
#ifndef W65816_OPS_H
#define W65816_OPS_H
#include "w65816.h"

#define NF 0x80
#define VF 0x40
#define MF 0x20
#define XF 0x10
#define DF 0x08
#define IF 0x04
#define ZF 0x02
#define CF 0x01
#define M8 (w65.p & MF)
#define X8 (w65.p & XF)

#define RD(a) w65_bus_read((a) & 0xFFFFFF)
#define WR(a, v) w65_bus_write((a) & 0xFFFFFF, (u8)(v))
#define IDLE() w65_bus_idle()

static inline u8 fetch8(void) { u8 v = RD((u32)w65.pb << 16 | w65.pc); w65.pc++; return v; }
static inline u16 fetch16(void) { u16 lo = fetch8(); return (u16)(lo | fetch8() << 8); }
static inline u32 fetch24(void) { u32 lo = fetch16(); return lo | (u32)fetch8() << 16; }

static inline u16 rd16(u32 a) { u16 lo = RD(a); return (u16)(lo | RD(a + 1) << 8); }
/* direct-page / stack data stays in bank 0 */
static inline u16 rd16z(u16 a) { u16 lo = RD(a); return (u16)(lo | RD((u16)(a + 1)) << 8); }

static inline void push8(u8 v) {
    WR(w65.s, v);
    if (w65.e) w65.s = (u16)(0x100 | ((w65.s - 1) & 0xFF)); else w65.s--;
}
static inline u8 pop8(void) {
    if (w65.e) w65.s = (u16)(0x100 | ((w65.s + 1) & 0xFF)); else w65.s++;
    return RD(w65.s);
}
static inline void push16(u16 v) { push8((u8)(v >> 8)); push8((u8)v); }
static inline u16 pop16(void) { u16 lo = pop8(); return (u16)(lo | pop8() << 8); }

static inline void nz8(u8 v) { w65.p = (u8)((w65.p & ~(NF | ZF)) | (v & 0x80) | (v ? 0 : ZF)); }
static inline void nz16(u16 v) { w65.p = (u8)((w65.p & ~(NF | ZF)) | ((v >> 8) & 0x80) | (v ? 0 : ZF)); }
static inline void setc(int c) { w65.p = (u8)((w65.p & ~CF) | (c ? CF : 0)); }
static inline void setv(int v) { w65.p = (u8)((w65.p & ~VF) | (v ? VF : 0)); }
static inline void seta8(u8 v) { w65.a = (u16)((w65.a & 0xFF00) | v); }

static inline void fix_p(void) {
    if (w65.e) w65.p |= MF | XF;
    if (w65.p & XF) { w65.x &= 0xFF; w65.y &= 0xFF; }
}

/* ------------------------------------------------- addressing modes
 * Operands are parameters: the interpreter passes fetched bytes, recompiled
 * code passes constants. */
static inline void dp_pen(void) { if (w65.d & 0xFF) IDLE(); }
static inline u32 a_dp_v(u8 o)  { dp_pen(); return (u16)(w65.d + o); }
static inline u32 a_dpx_v(u8 o) {
    dp_pen(); IDLE();
    if (w65.e && !(w65.d & 0xFF)) return (u16)(w65.d | (u8)(o + w65.x));
    return (u16)(w65.d + o + w65.x);
}
static inline u32 a_dpy_v(u8 o) {
    dp_pen(); IDLE();
    if (w65.e && !(w65.d & 0xFF)) return (u16)(w65.d | (u8)(o + w65.y));
    return (u16)(w65.d + o + w65.y);
}
static inline u32 a_abs_v(u16 b) { return (u32)w65.db << 16 | b; }
static inline u32 a_idx_v(u16 b, u16 idx, int wr) {
    u32 base = (u32)w65.db << 16 | b, a = (base + idx) & 0xFFFFFF;
    if (wr || !X8 || (base & 0xFF00) != (a & 0xFF00)) IDLE();
    return a;
}
static inline u32 a_alx_v(u32 v) { return (v + w65.x) & 0xFFFFFF; }
static inline u32 a_ind_v(u8 o) { u16 p = (u16)a_dp_v(o); return (u32)w65.db << 16 | rd16z(p); }
static inline u32 a_inx_v(u8 o) { u16 p = (u16)a_dpx_v(o); return (u32)w65.db << 16 | rd16z(p); }
static inline u32 a_iny_v(u8 o, int wr) {
    u16 p = (u16)a_dp_v(o);
    u32 base = (u32)w65.db << 16 | rd16z(p), a = (base + w65.y) & 0xFFFFFF;
    if (wr || !X8 || (base & 0xFF00) != (a & 0xFF00)) IDLE();
    return a;
}
static inline u32 a_inl_v(u8 o) { u16 p = (u16)a_dp_v(o); u32 lo = rd16z(p); return lo | (u32)RD((u16)(p + 2)) << 16; }
static inline u32 a_ily_v(u8 o) { return (a_inl_v(o) + w65.y) & 0xFFFFFF; }
static inline u32 a_sr_v(u8 o)  { IDLE(); return (u16)(w65.s + o); }
static inline u32 a_sry_v(u8 o) {
    u16 p = (u16)a_sr_v(o); u32 base = (u32)w65.db << 16 | rd16z(p);
    IDLE();
    return (base + w65.y) & 0xFFFFFF;
}

/* ------------------------------------------------------- ALU */
static inline void op_ora(u32 a) {
    if (M8) { u8 v = (u8)(w65.a | RD(a)); seta8(v); nz8(v); }
    else { w65.a |= rd16(a); nz16(w65.a); }
}
static inline void op_and(u32 a) {
    if (M8) { u8 v = (u8)(w65.a & RD(a)); seta8(v); nz8(v); }
    else { w65.a &= rd16(a); nz16(w65.a); }
}
static inline void op_eor(u32 a) {
    if (M8) { u8 v = (u8)(w65.a ^ RD(a)); seta8(v); nz8(v); }
    else { w65.a ^= rd16(a); nz16(w65.a); }
}
static inline void adc8(u8 v) {
    int c = w65.p & CF, al = w65.a & 0xFF, r;
    if (w65.p & DF) {
        r = (al & 0x0F) + (v & 0x0F) + c;
        if (r > 9) r += 6;
        c = r > 0x0F;
        r = (al & 0xF0) + (v & 0xF0) + (c << 4) + (r & 0x0F);
        setv(~(al ^ v) & (al ^ r) & 0x80);
        if (r > 0x9F) r += 0x60;
    } else {
        r = al + v + c;
        setv(~(al ^ v) & (al ^ r) & 0x80);
    }
    setc(r > 0xFF);
    seta8((u8)r); nz8((u8)r);
}
static inline void adc16(u16 v) {
    int c = w65.p & CF, a = w65.a, r;
    if (w65.p & DF) {
        r = (a & 0x000F) + (v & 0x000F) + c;
        if (r > 0x0009) r += 0x0006;
        c = r > 0x000F;
        r = (a & 0x00F0) + (v & 0x00F0) + (c << 4) + (r & 0x000F);
        if (r > 0x009F) r += 0x0060;
        c = r > 0x00FF;
        r = (a & 0x0F00) + (v & 0x0F00) + (c << 8) + (r & 0x00FF);
        if (r > 0x09FF) r += 0x0600;
        c = r > 0x0FFF;
        r = (a & 0xF000) + (v & 0xF000) + (c << 12) + (r & 0x0FFF);
        setv(~(a ^ v) & (a ^ r) & 0x8000);
        if (r > 0x9FFF) r += 0x6000;
    } else {
        r = a + v + c;
        setv(~(a ^ v) & (a ^ r) & 0x8000);
    }
    setc(r > 0xFFFF);
    w65.a = (u16)r; nz16((u16)r);
}
static inline void sbc8(u8 v) {
    int c = w65.p & CF, al = w65.a & 0xFF, r;
    u8 nv = (u8)~v;
    if (w65.p & DF) {
        r = (al & 0x0F) + (nv & 0x0F) + c;
        if (r <= 0x0F) r -= 6;
        c = r > 0x0F;
        r = (al & 0xF0) + (nv & 0xF0) + (c << 4) + (r & 0x0F);
        setv(~(al ^ nv) & (al ^ r) & 0x80);
        if (r <= 0xFF) r -= 0x60;
    } else {
        r = al + nv + c;
        setv(~(al ^ nv) & (al ^ r) & 0x80);
    }
    setc(r > 0xFF);
    seta8((u8)r); nz8((u8)r);
}
static inline void sbc16(u16 v) {
    int c = w65.p & CF, a = w65.a, r;
    u16 nv = (u16)~v;
    if (w65.p & DF) {
        r = (a & 0x000F) + (nv & 0x000F) + c;
        if (r <= 0x000F) r -= 0x0006;
        c = r > 0x000F;
        r = (a & 0x00F0) + (nv & 0x00F0) + (c << 4) + (r & 0x000F);
        if (r <= 0x00FF) r -= 0x0060;
        c = r > 0x00FF;
        r = (a & 0x0F00) + (nv & 0x0F00) + (c << 8) + (r & 0x00FF);
        if (r <= 0x0FFF) r -= 0x0600;
        c = r > 0x0FFF;
        r = (a & 0xF000) + (nv & 0xF000) + (c << 12) + (r & 0x0FFF);
        setv(~(a ^ nv) & (a ^ r) & 0x8000);
        if (r <= 0xFFFF) r -= 0x6000;
    } else {
        r = a + nv + c;
        setv(~(a ^ nv) & (a ^ r) & 0x8000);
    }
    setc(r > 0xFFFF);
    w65.a = (u16)r; nz16((u16)r);
}
static inline void op_adc(u32 a) { if (M8) adc8(RD(a)); else adc16(rd16(a)); }
static inline void op_sbc(u32 a) { if (M8) sbc8(RD(a)); else sbc16(rd16(a)); }
static inline void cmp8(u8 r, u8 v) { setc(r >= v); nz8((u8)(r - v)); }
static inline void cmp16(u16 r, u16 v) { setc(r >= v); nz16((u16)(r - v)); }
static inline void op_cmp(u32 a) { if (M8) cmp8((u8)w65.a, RD(a)); else cmp16(w65.a, rd16(a)); }
static inline void op_cpx(u32 a) { if (X8) cmp8((u8)w65.x, RD(a)); else cmp16(w65.x, rd16(a)); }
static inline void op_cpy(u32 a) { if (X8) cmp8((u8)w65.y, RD(a)); else cmp16(w65.y, rd16(a)); }
static inline void op_bit(u32 a) {
    if (M8) {
        u8 v = RD(a);
        w65.p = (u8)((w65.p & ~(NF | VF | ZF)) | (v & 0xC0) | ((v & w65.a & 0xFF) ? 0 : ZF));
    } else {
        u16 v = rd16(a);
        w65.p = (u8)((w65.p & ~(NF | VF | ZF)) | ((v >> 8) & 0xC0) | ((v & w65.a) ? 0 : ZF));
    }
}
static inline void op_lda(u32 a) { if (M8) { u8 v = RD(a); seta8(v); nz8(v); } else { w65.a = rd16(a); nz16(w65.a); } }
static inline void op_ldx(u32 a) { if (X8) { w65.x = RD(a); nz8((u8)w65.x); } else { w65.x = rd16(a); nz16(w65.x); } }
static inline void op_ldy(u32 a) { if (X8) { w65.y = RD(a); nz8((u8)w65.y); } else { w65.y = rd16(a); nz16(w65.y); } }
static inline void op_sta(u32 a) { if (M8) WR(a, w65.a); else { WR(a, w65.a); WR(a + 1, w65.a >> 8); } }
static inline void op_stx(u32 a) { if (X8) WR(a, w65.x); else { WR(a, w65.x); WR(a + 1, w65.x >> 8); } }
static inline void op_sty(u32 a) { if (X8) WR(a, w65.y); else { WR(a, w65.y); WR(a + 1, w65.y >> 8); } }
static inline void op_stz(u32 a) { WR(a, 0); if (!M8) WR(a + 1, 0); }

/* read-modify-write through one function so the bus pattern stays right */
typedef enum { R_ASL, R_LSR, R_ROL, R_ROR, R_INC, R_DEC, R_TSB, R_TRB } Rmw;
static inline u16 rmw_val(Rmw k, u16 v) {
    int w16 = !M8;
    u16 top = w16 ? 0x8000 : 0x80, mask = w16 ? 0xFFFF : 0xFF;
    int c = w65.p & CF;
    switch (k) {
    case R_ASL: setc(v & top); v = (u16)(v << 1); break;
    case R_LSR: setc(v & 1); v = (u16)(v >> 1); break;
    case R_ROL: { int o = (v & top) != 0; v = (u16)((v << 1) | c); setc(o); break; }
    case R_ROR: { int o = v & 1; v = (u16)((v >> 1) | (c ? top : 0)); setc(o); break; }
    case R_INC: v++; break;
    case R_DEC: v--; break;
    case R_TSB: case R_TRB: {
        u16 am = w65.a & mask;
        w65.p = (u8)((w65.p & ~ZF) | ((v & am) ? 0 : ZF));
        v = k == R_TSB ? (v | am) : (v & ~am);
        return v & mask;
    }
    }
    v &= mask;
    if (w16) nz16(v); else nz8((u8)v);
    return v;
}
static inline void rmw(Rmw k, u32 a) {
    if (M8) { u8 v = RD(a); IDLE(); WR(a, rmw_val(k, v)); }
    else { u16 v = rd16(a); IDLE(); v = rmw_val(k, v); WR(a + 1, v >> 8); WR(a, v); }
}
static inline void rmw_a(Rmw k) {
    IDLE();
    if (M8) seta8((u8)rmw_val(k, w65.a & 0xFF)); else w65.a = rmw_val(k, w65.a);
}

static inline void branch(int cond) {
    s8 o = (s8)fetch8();
    if (cond) {
        IDLE();
        u16 t = (u16)(w65.pc + o);
        if (w65.e && (t & 0xFF00) != (w65.pc & 0xFF00)) IDLE();
        w65.pc = t;
    }
}

static inline void interrupt(u16 vec_native, u16 vec_emu, int brk) {
    if (!w65.e) push8(w65.pb);
    push16(w65.pc);
    u8 p = w65.p;
    if (w65.e) p = brk ? (u8)(p | 0x10) : (u8)(p & ~0x10);
    push8(p);
    w65.p = (u8)((w65.p | IF) & ~DF);
    w65.pb = 0;
    w65.pc = rd16(w65.e ? vec_emu : vec_native);
}


#endif

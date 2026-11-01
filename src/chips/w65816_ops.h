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

/* Recompiled code runs only in native mode and passes the M and X flags as
 * constants: `m` is 1 for 8-bit A, `xf` for 8-bit index, so those tests compile
 * away. The interpreter gets the same functions through the macros at the end. */
#define INL static inline __attribute__((always_inline))
#ifndef RD
#define RD(a) w65_bus_read((a) & 0xFFFFFF)
#define WR(a, v) w65_bus_write((a) & 0xFFFFFF, (u8)(v))
#define IDLE() w65_bus_idle()
#endif
#ifdef W65_NATIVE
#define E_MODE 0
#else
#define E_MODE w65.e
#endif

INL u8 fetch8(void) { u8 v = RD((u32)w65.pb << 16 | w65.pc); w65.pc++; return v; }
INL u16 fetch16(void) { u16 lo = fetch8(); return (u16)(lo | fetch8() << 8); }
INL u32 fetch24(void) { u32 lo = fetch16(); return lo | (u32)fetch8() << 16; }

INL u16 rd16(u32 a) { u16 lo = RD(a); return (u16)(lo | RD(a + 1) << 8); }
/* direct-page / stack data stays in bank 0 */
INL u16 rd16z(u16 a) { u16 lo = RD(a); return (u16)(lo | RD((u16)(a + 1)) << 8); }

INL void push8(u8 v) {
    WR(w65.s, v);
    if (E_MODE) w65.s = (u16)(0x100 | ((w65.s - 1) & 0xFF)); else w65.s--;
}
INL u8 pop8(void) {
    if (E_MODE) w65.s = (u16)(0x100 | ((w65.s + 1) & 0xFF)); else w65.s++;
    return RD(w65.s);
}
INL void push16(u16 v) { push8((u8)(v >> 8)); push8((u8)v); }
INL u16 pop16(void) { u16 lo = pop8(); return (u16)(lo | pop8() << 8); }

INL void nz8(u8 v) { w65.p = (u8)((w65.p & ~(NF | ZF)) | (v & 0x80) | (v ? 0 : ZF)); }
INL void nz16(u16 v) { w65.p = (u8)((w65.p & ~(NF | ZF)) | ((v >> 8) & 0x80) | (v ? 0 : ZF)); }
INL void setc(int c) { w65.p = (u8)((w65.p & ~CF) | (c ? CF : 0)); }
INL void setv(int v) { w65.p = (u8)((w65.p & ~VF) | (v ? VF : 0)); }
INL void seta8(u8 v) { w65.a = (u16)((w65.a & 0xFF00) | v); }

INL void fix_p(void) {
    if (E_MODE) w65.p |= MF | XF;
    if (w65.p & XF) { w65.x &= 0xFF; w65.y &= 0xFF; }
}

/* ------------------------------------------------- addressing modes
 * Operands are parameters: the interpreter passes fetched bytes, recompiled
 * code passes constants. */
INL void dp_pen(void) { if (w65.d & 0xFF) IDLE(); }
INL u32 a_dp_v(u8 o)  { dp_pen(); return (u16)(w65.d + o); }
INL u32 a_dpx_v(u8 o) {
    dp_pen(); IDLE();
    if (E_MODE && !(w65.d & 0xFF)) return (u16)(w65.d | (u8)(o + w65.x));
    return (u16)(w65.d + o + w65.x);
}
INL u32 a_dpy_v(u8 o) {
    dp_pen(); IDLE();
    if (E_MODE && !(w65.d & 0xFF)) return (u16)(w65.d | (u8)(o + w65.y));
    return (u16)(w65.d + o + w65.y);
}
INL u32 a_abs_v(u16 b) { return (u32)w65.db << 16 | b; }
INL u32 a_idx_(int xf, u16 b, u16 idx, int wr) {
    u32 base = (u32)w65.db << 16 | b, a = (base + idx) & 0xFFFFFF;
    if (wr || !xf || (base & 0xFF00) != (a & 0xFF00)) IDLE();
    return a;
}
INL u32 a_alx_v(u32 v) { return (v + w65.x) & 0xFFFFFF; }
INL u32 a_ind_v(u8 o) { u16 p = (u16)a_dp_v(o); return (u32)w65.db << 16 | rd16z(p); }
INL u32 a_inx_v(u8 o) { u16 p = (u16)a_dpx_v(o); return (u32)w65.db << 16 | rd16z(p); }
INL u32 a_iny_(int xf, u8 o, int wr) {
    u16 p = (u16)a_dp_v(o);
    u32 base = (u32)w65.db << 16 | rd16z(p), a = (base + w65.y) & 0xFFFFFF;
    if (wr || !xf || (base & 0xFF00) != (a & 0xFF00)) IDLE();
    return a;
}
INL u32 a_inl_v(u8 o) { u16 p = (u16)a_dp_v(o); u32 lo = rd16z(p); return lo | (u32)RD((u16)(p + 2)) << 16; }
INL u32 a_ily_v(u8 o) { return (a_inl_v(o) + w65.y) & 0xFFFFFF; }
INL u32 a_sr_v(u8 o)  { IDLE(); return (u16)(w65.s + o); }
INL u32 a_sry_v(u8 o) {
    u16 p = (u16)a_sr_v(o); u32 base = (u32)w65.db << 16 | rd16z(p);
    IDLE();
    return (base + w65.y) & 0xFFFFFF;
}

/* ------------------------------------------------------- ALU */
INL void op_ora_(int m, u32 a) {
    if (m) { u8 v = (u8)(w65.a | RD(a)); seta8(v); nz8(v); }
    else { w65.a |= rd16(a); nz16(w65.a); }
}
INL void op_and_(int m, u32 a) {
    if (m) { u8 v = (u8)(w65.a & RD(a)); seta8(v); nz8(v); }
    else { w65.a &= rd16(a); nz16(w65.a); }
}
INL void op_eor_(int m, u32 a) {
    if (m) { u8 v = (u8)(w65.a ^ RD(a)); seta8(v); nz8(v); }
    else { w65.a ^= rd16(a); nz16(w65.a); }
}
INL void adc8(u8 v) {
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
INL void adc16(u16 v) {
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
INL void sbc8(u8 v) {
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
INL void sbc16(u16 v) {
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
INL void op_adc_(int m, u32 a) { if (m) adc8(RD(a)); else adc16(rd16(a)); }
INL void op_sbc_(int m, u32 a) { if (m) sbc8(RD(a)); else sbc16(rd16(a)); }
INL void cmp8(u8 r, u8 v) { setc(r >= v); nz8((u8)(r - v)); }
INL void cmp16(u16 r, u16 v) { setc(r >= v); nz16((u16)(r - v)); }
INL void op_cmp_(int m, u32 a) { if (m) cmp8((u8)w65.a, RD(a)); else cmp16(w65.a, rd16(a)); }
INL void op_cpx_(int xf, u32 a) { if (xf) cmp8((u8)w65.x, RD(a)); else cmp16(w65.x, rd16(a)); }
INL void op_cpy_(int xf, u32 a) { if (xf) cmp8((u8)w65.y, RD(a)); else cmp16(w65.y, rd16(a)); }
INL void op_bit_(int m, u32 a) {
    if (m) {
        u8 v = RD(a);
        w65.p = (u8)((w65.p & ~(NF | VF | ZF)) | (v & 0xC0) | ((v & w65.a & 0xFF) ? 0 : ZF));
    } else {
        u16 v = rd16(a);
        w65.p = (u8)((w65.p & ~(NF | VF | ZF)) | ((v >> 8) & 0xC0) | ((v & w65.a) ? 0 : ZF));
    }
}
INL void op_lda_(int m, u32 a) { if (m) { u8 v = RD(a); seta8(v); nz8(v); } else { w65.a = rd16(a); nz16(w65.a); } }
INL void op_ldx_(int xf, u32 a) { if (xf) { w65.x = RD(a); nz8((u8)w65.x); } else { w65.x = rd16(a); nz16(w65.x); } }
INL void op_ldy_(int xf, u32 a) { if (xf) { w65.y = RD(a); nz8((u8)w65.y); } else { w65.y = rd16(a); nz16(w65.y); } }
INL void op_sta_(int m, u32 a) { if (m) WR(a, w65.a); else { WR(a, w65.a); WR(a + 1, w65.a >> 8); } }
INL void op_stx_(int xf, u32 a) { if (xf) WR(a, w65.x); else { WR(a, w65.x); WR(a + 1, w65.x >> 8); } }
INL void op_sty_(int xf, u32 a) { if (xf) WR(a, w65.y); else { WR(a, w65.y); WR(a + 1, w65.y >> 8); } }
INL void op_stz_(int m, u32 a) { WR(a, 0); if (!m) WR(a + 1, 0); }

/* read-modify-write through one function so the bus pattern stays right */
typedef enum { R_ASL, R_LSR, R_ROL, R_ROR, R_INC, R_DEC, R_TSB, R_TRB } Rmw;
INL u16 rmw_val_(int m, Rmw k, u16 v) {
    int w16 = !m;
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
INL void rmw_(int m, Rmw k, u32 a) {
    if (m) { u8 v = RD(a); IDLE(); WR(a, rmw_val_(m, k, v)); }
    else { u16 v = rd16(a); IDLE(); v = rmw_val_(m, k, v); WR(a + 1, v >> 8); WR(a, v); }
}
INL void rmw_a_(int m, Rmw k) {
    IDLE();
    if (m) seta8((u8)rmw_val_(m, k, w65.a & 0xFF)); else w65.a = rmw_val_(m, k, w65.a);
}

INL void branch(int cond) {
    s8 o = (s8)fetch8();
    if (cond) {
        IDLE();
        u16 t = (u16)(w65.pc + o);
        if (w65.e && (t & 0xFF00) != (w65.pc & 0xFF00)) IDLE();
        w65.pc = t;
    }
}

INL void interrupt(u16 vec_native, u16 vec_emu, int brk) {
    if (!w65.e) push8(w65.pb);
    push16(w65.pc);
    u8 p = w65.p;
    if (w65.e) p = brk ? (u8)(p | 0x10) : (u8)(p & ~0x10);
    push8(p);
    w65.p = (u8)((w65.p | IF) & ~DF);
    w65.pb = 0;
    w65.pc = rd16(w65.e ? vec_emu : vec_native);
}

/* ------------------------------------------- stack, transfers, index, flags */
INL void op_pha_(int m) { IDLE(); if (m) push8((u8)w65.a); else push16(w65.a); }
INL void op_pla_(int m) { IDLE(); IDLE(); if (m) { u8 v = pop8(); seta8(v); nz8(v); } else { w65.a = pop16(); nz16(w65.a); } }
INL void op_phx_(int xf) { IDLE(); if (xf) push8((u8)w65.x); else push16(w65.x); }
INL void op_plx_(int xf) { IDLE(); IDLE(); if (xf) { w65.x = pop8(); nz8((u8)w65.x); } else { w65.x = pop16(); nz16(w65.x); } }
INL void op_phy_(int xf) { IDLE(); if (xf) push8((u8)w65.y); else push16(w65.y); }
INL void op_ply_(int xf) { IDLE(); IDLE(); if (xf) { w65.y = pop8(); nz8((u8)w65.y); } else { w65.y = pop16(); nz16(w65.y); } }
INL void op_php(void) { IDLE(); push8(w65.p); }
INL void op_phb(void) { IDLE(); push8(w65.db); }
INL void op_plb(void) { IDLE(); IDLE(); w65.db = pop8(); nz8(w65.db); }
INL void op_phd(void) { IDLE(); push16(w65.d); }
INL void op_pld(void) { IDLE(); IDLE(); w65.d = pop16(); nz16(w65.d); }
INL void op_phk(void) { IDLE(); push8(w65.pb); }
INL void op_pei(u8 o) { u16 p = (u16)a_dp_v(o); push16(rd16z(p)); }
INL void op_tax_(int xf) { IDLE(); if (xf) { w65.x = w65.a & 0xFF; nz8((u8)w65.x); } else { w65.x = w65.a; nz16(w65.x); } }
INL void op_tay_(int xf) { IDLE(); if (xf) { w65.y = w65.a & 0xFF; nz8((u8)w65.y); } else { w65.y = w65.a; nz16(w65.y); } }
INL void op_txa_(int m) { IDLE(); if (m) { seta8((u8)w65.x); nz8((u8)w65.x); } else { w65.a = w65.x; nz16(w65.a); } }
INL void op_tya_(int m) { IDLE(); if (m) { seta8((u8)w65.y); nz8((u8)w65.y); } else { w65.a = w65.y; nz16(w65.a); } }
INL void op_txy_(int xf) { IDLE(); w65.y = w65.x; if (xf) nz8((u8)w65.y); else nz16(w65.y); }
INL void op_tyx_(int xf) { IDLE(); w65.x = w65.y; if (xf) nz8((u8)w65.x); else nz16(w65.x); }
INL void op_tsx_(int xf) { IDLE(); if (xf) { w65.x = w65.s & 0xFF; nz8((u8)w65.x); } else { w65.x = w65.s; nz16(w65.x); } }
INL void op_txs(void) { IDLE(); w65.s = E_MODE ? (u16)(0x100 | (w65.x & 0xFF)) : w65.x; }
INL void op_tcd(void) { IDLE(); w65.d = w65.a; nz16(w65.d); }
INL void op_tdc(void) { IDLE(); w65.a = w65.d; nz16(w65.a); }
INL void op_tcs(void) { IDLE(); w65.s = E_MODE ? (u16)(0x100 | (w65.a & 0xFF)) : w65.a; }
INL void op_tsc(void) { IDLE(); w65.a = w65.s; nz16(w65.a); }
INL void op_xba(void) { IDLE(); IDLE(); w65.a = (u16)(w65.a >> 8 | w65.a << 8); nz8((u8)w65.a); }
INL void op_inx_(int xf) { IDLE(); if (xf) { w65.x = (u8)(w65.x + 1); nz8((u8)w65.x); } else { w65.x++; nz16(w65.x); } }
INL void op_dex_(int xf) { IDLE(); if (xf) { w65.x = (u8)(w65.x - 1); nz8((u8)w65.x); } else { w65.x--; nz16(w65.x); } }
INL void op_iny_(int xf) { IDLE(); if (xf) { w65.y = (u8)(w65.y + 1); nz8((u8)w65.y); } else { w65.y++; nz16(w65.y); } }
INL void op_dey_(int xf) { IDLE(); if (xf) { w65.y = (u8)(w65.y - 1); nz8((u8)w65.y); } else { w65.y--; nz16(w65.y); } }
INL void op_rep(u8 v) { IDLE(); w65.p &= (u8)~v; fix_p(); }
INL void op_sep(u8 v) { IDLE(); w65.p |= v; fix_p(); }
INL void op_rts(void) { IDLE(); IDLE(); w65.pc = (u16)(pop16() + 1); IDLE(); }
INL void op_rtl(void) { IDLE(); IDLE(); u16 t = pop16(); w65.pb = pop8(); w65.pc = (u16)(t + 1); }
INL void op_bit_imm8(u8 v) { w65.p = (u8)((w65.p & ~ZF) | ((v & w65.a & 0xFF) ? 0 : ZF)); }
INL void op_bit_imm16(u16 v) { w65.p = (u8)((w65.p & ~ZF) | ((v & w65.a) ? 0 : ZF)); }

#ifndef W65_NATIVE
#define op_ora(a) op_ora_(M8, a)
#define op_and(a) op_and_(M8, a)
#define op_eor(a) op_eor_(M8, a)
#define op_adc(a) op_adc_(M8, a)
#define op_sbc(a) op_sbc_(M8, a)
#define op_cmp(a) op_cmp_(M8, a)
#define op_bit(a) op_bit_(M8, a)
#define op_lda(a) op_lda_(M8, a)
#define op_sta(a) op_sta_(M8, a)
#define op_stz(a) op_stz_(M8, a)
#define rmw(k, a) rmw_(M8, k, a)
#define rmw_a(k) rmw_a_(M8, k)
#define op_cpx(a) op_cpx_(X8, a)
#define op_cpy(a) op_cpy_(X8, a)
#define op_ldx(a) op_ldx_(X8, a)
#define op_ldy(a) op_ldy_(X8, a)
#define op_stx(a) op_stx_(X8, a)
#define op_sty(a) op_sty_(X8, a)
#define a_idx_v(b, i, w) a_idx_(X8, b, i, w)
#define a_iny_v(o, w) a_iny_(X8, o, w)
#endif

#endif

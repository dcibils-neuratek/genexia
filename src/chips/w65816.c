/*
 * w65816.c - WDC 65C816 interpreter. See w65816.h.
 * Every bus access and internal cycle goes through the machine's callbacks,
 * which is where memory speed and timing are accounted for.
 */
#include "w65816.h"

EMU_STATE W65816 w65;

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

static void fix_p(void) {
    if (w65.e) w65.p |= MF | XF;
    if (w65.p & XF) { w65.x &= 0xFF; w65.y &= 0xFF; }
}

/* ------------------------------------------------- addressing modes */
static inline void dp_pen(void) { if (w65.d & 0xFF) IDLE(); }
static u32 a_dp(void)  { u8 o = fetch8(); dp_pen(); return (u16)(w65.d + o); }
static u32 a_dpx(void) {
    u8 o = fetch8(); dp_pen(); IDLE();
    if (w65.e && !(w65.d & 0xFF)) return (u16)(w65.d | (u8)(o + w65.x));
    return (u16)(w65.d + o + w65.x);
}
static u32 a_dpy(void) {
    u8 o = fetch8(); dp_pen(); IDLE();
    if (w65.e && !(w65.d & 0xFF)) return (u16)(w65.d | (u8)(o + w65.y));
    return (u16)(w65.d + o + w65.y);
}
static u32 a_abs(void) { u16 b = fetch16(); return (u32)w65.db << 16 | b; }
static u32 a_idx(u16 idx, int wr) {
    u16 b = fetch16();
    u32 base = (u32)w65.db << 16 | b, a = (base + idx) & 0xFFFFFF;
    if (wr || !X8 || (base & 0xFF00) != (a & 0xFF00)) IDLE();
    return a;
}
static u32 a_abl(void) { return fetch24(); }
static u32 a_alx(void) { return (fetch24() + w65.x) & 0xFFFFFF; }
static u32 a_ind(void) { u16 p = (u16)a_dp(); return (u32)w65.db << 16 | rd16z(p); }
static u32 a_inx(void) { u16 p = (u16)a_dpx(); return (u32)w65.db << 16 | rd16z(p); }
static u32 a_iny(int wr) {
    u16 p = (u16)a_dp();
    u32 base = (u32)w65.db << 16 | rd16z(p), a = (base + w65.y) & 0xFFFFFF;
    if (wr || !X8 || (base & 0xFF00) != (a & 0xFF00)) IDLE();
    return a;
}
static u32 a_inl(void) { u16 p = (u16)a_dp(); u32 lo = rd16z(p); return lo | (u32)RD((u16)(p + 2)) << 16; }
static u32 a_ily(void) { return (a_inl() + w65.y) & 0xFFFFFF; }
static u32 a_sr(void)  { u8 o = fetch8(); IDLE(); return (u16)(w65.s + o); }
static u32 a_sry(void) {
    u16 p = (u16)a_sr(); u32 base = (u32)w65.db << 16 | rd16z(p);
    IDLE();
    return (base + w65.y) & 0xFFFFFF;
}

/* ------------------------------------------------------- ALU */
static void op_ora(u32 a) {
    if (M8) { u8 v = (u8)(w65.a | RD(a)); seta8(v); nz8(v); }
    else { w65.a |= rd16(a); nz16(w65.a); }
}
static void op_and(u32 a) {
    if (M8) { u8 v = (u8)(w65.a & RD(a)); seta8(v); nz8(v); }
    else { w65.a &= rd16(a); nz16(w65.a); }
}
static void op_eor(u32 a) {
    if (M8) { u8 v = (u8)(w65.a ^ RD(a)); seta8(v); nz8(v); }
    else { w65.a ^= rd16(a); nz16(w65.a); }
}
static void adc8(u8 v) {
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
static void adc16(u16 v) {
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
static void sbc8(u8 v) {
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
static void sbc16(u16 v) {
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
static void op_adc(u32 a) { if (M8) adc8(RD(a)); else adc16(rd16(a)); }
static void op_sbc(u32 a) { if (M8) sbc8(RD(a)); else sbc16(rd16(a)); }
static void cmp8(u8 r, u8 v) { setc(r >= v); nz8((u8)(r - v)); }
static void cmp16(u16 r, u16 v) { setc(r >= v); nz16((u16)(r - v)); }
static void op_cmp(u32 a) { if (M8) cmp8((u8)w65.a, RD(a)); else cmp16(w65.a, rd16(a)); }
static void op_cpx(u32 a) { if (X8) cmp8((u8)w65.x, RD(a)); else cmp16(w65.x, rd16(a)); }
static void op_cpy(u32 a) { if (X8) cmp8((u8)w65.y, RD(a)); else cmp16(w65.y, rd16(a)); }
static void op_bit(u32 a) {
    if (M8) {
        u8 v = RD(a);
        w65.p = (u8)((w65.p & ~(NF | VF | ZF)) | (v & 0xC0) | ((v & w65.a & 0xFF) ? 0 : ZF));
    } else {
        u16 v = rd16(a);
        w65.p = (u8)((w65.p & ~(NF | VF | ZF)) | ((v >> 8) & 0xC0) | ((v & w65.a) ? 0 : ZF));
    }
}
static void op_lda(u32 a) { if (M8) { u8 v = RD(a); seta8(v); nz8(v); } else { w65.a = rd16(a); nz16(w65.a); } }
static void op_ldx(u32 a) { if (X8) { w65.x = RD(a); nz8((u8)w65.x); } else { w65.x = rd16(a); nz16(w65.x); } }
static void op_ldy(u32 a) { if (X8) { w65.y = RD(a); nz8((u8)w65.y); } else { w65.y = rd16(a); nz16(w65.y); } }
static void op_sta(u32 a) { if (M8) WR(a, w65.a); else { WR(a, w65.a); WR(a + 1, w65.a >> 8); } }
static void op_stx(u32 a) { if (X8) WR(a, w65.x); else { WR(a, w65.x); WR(a + 1, w65.x >> 8); } }
static void op_sty(u32 a) { if (X8) WR(a, w65.y); else { WR(a, w65.y); WR(a + 1, w65.y >> 8); } }
static void op_stz(u32 a) { WR(a, 0); if (!M8) WR(a + 1, 0); }

/* read-modify-write through one function so the bus pattern stays right */
typedef enum { R_ASL, R_LSR, R_ROL, R_ROR, R_INC, R_DEC, R_TSB, R_TRB } Rmw;
static u16 rmw_val(Rmw k, u16 v) {
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
static void rmw(Rmw k, u32 a) {
    if (M8) { u8 v = RD(a); IDLE(); WR(a, rmw_val(k, v)); }
    else { u16 v = rd16(a); IDLE(); v = rmw_val(k, v); WR(a + 1, v >> 8); WR(a, v); }
}
static void rmw_a(Rmw k) {
    IDLE();
    if (M8) seta8((u8)rmw_val(k, w65.a & 0xFF)); else w65.a = rmw_val(k, w65.a);
}

static void branch(int cond) {
    s8 o = (s8)fetch8();
    if (cond) {
        IDLE();
        u16 t = (u16)(w65.pc + o);
        if (w65.e && (t & 0xFF00) != (w65.pc & 0xFF00)) IDLE();
        w65.pc = t;
    }
}

static void interrupt(u16 vec_native, u16 vec_emu, int brk) {
    if (!w65.e) push8(w65.pb);
    push16(w65.pc);
    u8 p = w65.p;
    if (w65.e) p = brk ? (u8)(p | 0x10) : (u8)(p & ~0x10);
    push8(p);
    w65.p = (u8)((w65.p | IF) & ~DF);
    w65.pb = 0;
    w65.pc = rd16(w65.e ? vec_emu : vec_native);
}

void w65_reset(void) {
    w65.e = 1; w65.d = 0; w65.db = 0; w65.pb = 0; w65.s = 0x01FF;
    w65.p = MF | XF | IF;
    w65.x &= 0xFF; w65.y &= 0xFF;
    w65.stopped = w65.waiting = 0; w65.nmi_pending = 0;
    w65.pc = rd16(0xFFFC);
}

void w65_step(void) {
    if (w65.stopped) { IDLE(); return; }
    if (w65.nmi_pending) {
        w65.nmi_pending = 0; w65.waiting = 0;
        IDLE(); IDLE();
        interrupt(0xFFEA, 0xFFFA, 0);
        return;
    }
    if (w65.irq_line) {
        w65.waiting = 0;
        if (!(w65.p & IF)) { IDLE(); IDLE(); interrupt(0xFFEE, 0xFFFE, 0); return; }
    }
    if (w65.waiting) { IDLE(); return; }

    u8 op = fetch8();
    u32 a;
    switch (op) {
    /* ORA */
    case 0x01: op_ora(a_inx()); break; case 0x03: op_ora(a_sr()); break;
    case 0x05: op_ora(a_dp()); break;  case 0x07: op_ora(a_inl()); break;
    case 0x0D: op_ora(a_abs()); break; case 0x0F: op_ora(a_abl()); break;
    case 0x11: op_ora(a_iny(0)); break; case 0x12: op_ora(a_ind()); break;
    case 0x13: op_ora(a_sry()); break; case 0x15: op_ora(a_dpx()); break;
    case 0x17: op_ora(a_ily()); break; case 0x19: op_ora(a_idx(w65.y, 0)); break;
    case 0x1D: op_ora(a_idx(w65.x, 0)); break; case 0x1F: op_ora(a_alx()); break;
    case 0x09: if (M8) { u8 v = (u8)(w65.a | fetch8()); seta8(v); nz8(v); } else { w65.a |= fetch16(); nz16(w65.a); } break;
    /* AND */
    case 0x21: op_and(a_inx()); break; case 0x23: op_and(a_sr()); break;
    case 0x25: op_and(a_dp()); break;  case 0x27: op_and(a_inl()); break;
    case 0x2D: op_and(a_abs()); break; case 0x2F: op_and(a_abl()); break;
    case 0x31: op_and(a_iny(0)); break; case 0x32: op_and(a_ind()); break;
    case 0x33: op_and(a_sry()); break; case 0x35: op_and(a_dpx()); break;
    case 0x37: op_and(a_ily()); break; case 0x39: op_and(a_idx(w65.y, 0)); break;
    case 0x3D: op_and(a_idx(w65.x, 0)); break; case 0x3F: op_and(a_alx()); break;
    case 0x29: if (M8) { u8 v = (u8)(w65.a & fetch8()); seta8(v); nz8(v); } else { w65.a &= fetch16(); nz16(w65.a); } break;
    /* EOR */
    case 0x41: op_eor(a_inx()); break; case 0x43: op_eor(a_sr()); break;
    case 0x45: op_eor(a_dp()); break;  case 0x47: op_eor(a_inl()); break;
    case 0x4D: op_eor(a_abs()); break; case 0x4F: op_eor(a_abl()); break;
    case 0x51: op_eor(a_iny(0)); break; case 0x52: op_eor(a_ind()); break;
    case 0x53: op_eor(a_sry()); break; case 0x55: op_eor(a_dpx()); break;
    case 0x57: op_eor(a_ily()); break; case 0x59: op_eor(a_idx(w65.y, 0)); break;
    case 0x5D: op_eor(a_idx(w65.x, 0)); break; case 0x5F: op_eor(a_alx()); break;
    case 0x49: if (M8) { u8 v = (u8)(w65.a ^ fetch8()); seta8(v); nz8(v); } else { w65.a ^= fetch16(); nz16(w65.a); } break;
    /* ADC */
    case 0x61: op_adc(a_inx()); break; case 0x63: op_adc(a_sr()); break;
    case 0x65: op_adc(a_dp()); break;  case 0x67: op_adc(a_inl()); break;
    case 0x6D: op_adc(a_abs()); break; case 0x6F: op_adc(a_abl()); break;
    case 0x71: op_adc(a_iny(0)); break; case 0x72: op_adc(a_ind()); break;
    case 0x73: op_adc(a_sry()); break; case 0x75: op_adc(a_dpx()); break;
    case 0x77: op_adc(a_ily()); break; case 0x79: op_adc(a_idx(w65.y, 0)); break;
    case 0x7D: op_adc(a_idx(w65.x, 0)); break; case 0x7F: op_adc(a_alx()); break;
    case 0x69: if (M8) adc8(fetch8()); else adc16(fetch16()); break;
    /* STA */
    case 0x81: op_sta(a_inx()); break; case 0x83: op_sta(a_sr()); break;
    case 0x85: op_sta(a_dp()); break;  case 0x87: op_sta(a_inl()); break;
    case 0x8D: op_sta(a_abs()); break; case 0x8F: op_sta(a_abl()); break;
    case 0x91: op_sta(a_iny(1)); break; case 0x92: op_sta(a_ind()); break;
    case 0x93: op_sta(a_sry()); break; case 0x95: op_sta(a_dpx()); break;
    case 0x97: op_sta(a_ily()); break; case 0x99: op_sta(a_idx(w65.y, 1)); break;
    case 0x9D: op_sta(a_idx(w65.x, 1)); break; case 0x9F: op_sta(a_alx()); break;
    /* LDA */
    case 0xA1: op_lda(a_inx()); break; case 0xA3: op_lda(a_sr()); break;
    case 0xA5: op_lda(a_dp()); break;  case 0xA7: op_lda(a_inl()); break;
    case 0xAD: op_lda(a_abs()); break; case 0xAF: op_lda(a_abl()); break;
    case 0xB1: op_lda(a_iny(0)); break; case 0xB2: op_lda(a_ind()); break;
    case 0xB3: op_lda(a_sry()); break; case 0xB5: op_lda(a_dpx()); break;
    case 0xB7: op_lda(a_ily()); break; case 0xB9: op_lda(a_idx(w65.y, 0)); break;
    case 0xBD: op_lda(a_idx(w65.x, 0)); break; case 0xBF: op_lda(a_alx()); break;
    case 0xA9: if (M8) { u8 v = fetch8(); seta8(v); nz8(v); } else { w65.a = fetch16(); nz16(w65.a); } break;
    /* CMP */
    case 0xC1: op_cmp(a_inx()); break; case 0xC3: op_cmp(a_sr()); break;
    case 0xC5: op_cmp(a_dp()); break;  case 0xC7: op_cmp(a_inl()); break;
    case 0xCD: op_cmp(a_abs()); break; case 0xCF: op_cmp(a_abl()); break;
    case 0xD1: op_cmp(a_iny(0)); break; case 0xD2: op_cmp(a_ind()); break;
    case 0xD3: op_cmp(a_sry()); break; case 0xD5: op_cmp(a_dpx()); break;
    case 0xD7: op_cmp(a_ily()); break; case 0xD9: op_cmp(a_idx(w65.y, 0)); break;
    case 0xDD: op_cmp(a_idx(w65.x, 0)); break; case 0xDF: op_cmp(a_alx()); break;
    case 0xC9: if (M8) cmp8((u8)w65.a, fetch8()); else cmp16(w65.a, fetch16()); break;
    /* SBC */
    case 0xE1: op_sbc(a_inx()); break; case 0xE3: op_sbc(a_sr()); break;
    case 0xE5: op_sbc(a_dp()); break;  case 0xE7: op_sbc(a_inl()); break;
    case 0xED: op_sbc(a_abs()); break; case 0xEF: op_sbc(a_abl()); break;
    case 0xF1: op_sbc(a_iny(0)); break; case 0xF2: op_sbc(a_ind()); break;
    case 0xF3: op_sbc(a_sry()); break; case 0xF5: op_sbc(a_dpx()); break;
    case 0xF7: op_sbc(a_ily()); break; case 0xF9: op_sbc(a_idx(w65.y, 0)); break;
    case 0xFD: op_sbc(a_idx(w65.x, 0)); break; case 0xFF: op_sbc(a_alx()); break;
    case 0xE9: if (M8) sbc8(fetch8()); else sbc16(fetch16()); break;
    /* shifts, INC, DEC, TSB, TRB */
    case 0x06: rmw(R_ASL, a_dp()); break; case 0x0E: rmw(R_ASL, a_abs()); break;
    case 0x16: rmw(R_ASL, a_dpx()); break; case 0x1E: rmw(R_ASL, a_idx(w65.x, 1)); break;
    case 0x0A: rmw_a(R_ASL); break;
    case 0x46: rmw(R_LSR, a_dp()); break; case 0x4E: rmw(R_LSR, a_abs()); break;
    case 0x56: rmw(R_LSR, a_dpx()); break; case 0x5E: rmw(R_LSR, a_idx(w65.x, 1)); break;
    case 0x4A: rmw_a(R_LSR); break;
    case 0x26: rmw(R_ROL, a_dp()); break; case 0x2E: rmw(R_ROL, a_abs()); break;
    case 0x36: rmw(R_ROL, a_dpx()); break; case 0x3E: rmw(R_ROL, a_idx(w65.x, 1)); break;
    case 0x2A: rmw_a(R_ROL); break;
    case 0x66: rmw(R_ROR, a_dp()); break; case 0x6E: rmw(R_ROR, a_abs()); break;
    case 0x76: rmw(R_ROR, a_dpx()); break; case 0x7E: rmw(R_ROR, a_idx(w65.x, 1)); break;
    case 0x6A: rmw_a(R_ROR); break;
    case 0xE6: rmw(R_INC, a_dp()); break; case 0xEE: rmw(R_INC, a_abs()); break;
    case 0xF6: rmw(R_INC, a_dpx()); break; case 0xFE: rmw(R_INC, a_idx(w65.x, 1)); break;
    case 0x1A: rmw_a(R_INC); break;
    case 0xC6: rmw(R_DEC, a_dp()); break; case 0xCE: rmw(R_DEC, a_abs()); break;
    case 0xD6: rmw(R_DEC, a_dpx()); break; case 0xDE: rmw(R_DEC, a_idx(w65.x, 1)); break;
    case 0x3A: rmw_a(R_DEC); break;
    case 0x04: rmw(R_TSB, a_dp()); break; case 0x0C: rmw(R_TSB, a_abs()); break;
    case 0x14: rmw(R_TRB, a_dp()); break; case 0x1C: rmw(R_TRB, a_abs()); break;
    /* BIT */
    case 0x24: op_bit(a_dp()); break; case 0x2C: op_bit(a_abs()); break;
    case 0x34: op_bit(a_dpx()); break; case 0x3C: op_bit(a_idx(w65.x, 0)); break;
    case 0x89:   /* immediate BIT only changes Z */
        if (M8) { u8 v = fetch8(); w65.p = (u8)((w65.p & ~ZF) | ((v & w65.a & 0xFF) ? 0 : ZF)); }
        else { u16 v = fetch16(); w65.p = (u8)((w65.p & ~ZF) | ((v & w65.a) ? 0 : ZF)); }
        break;
    /* X, Y load/store/compare */
    case 0xA2: if (X8) { w65.x = fetch8(); nz8((u8)w65.x); } else { w65.x = fetch16(); nz16(w65.x); } break;
    case 0xA6: op_ldx(a_dp()); break; case 0xAE: op_ldx(a_abs()); break;
    case 0xB6: op_ldx(a_dpy()); break; case 0xBE: op_ldx(a_idx(w65.y, 0)); break;
    case 0xA0: if (X8) { w65.y = fetch8(); nz8((u8)w65.y); } else { w65.y = fetch16(); nz16(w65.y); } break;
    case 0xA4: op_ldy(a_dp()); break; case 0xAC: op_ldy(a_abs()); break;
    case 0xB4: op_ldy(a_dpx()); break; case 0xBC: op_ldy(a_idx(w65.x, 0)); break;
    case 0x86: op_stx(a_dp()); break; case 0x8E: op_stx(a_abs()); break; case 0x96: op_stx(a_dpy()); break;
    case 0x84: op_sty(a_dp()); break; case 0x8C: op_sty(a_abs()); break; case 0x94: op_sty(a_dpx()); break;
    case 0x64: op_stz(a_dp()); break; case 0x74: op_stz(a_dpx()); break;
    case 0x9C: op_stz(a_abs()); break; case 0x9E: op_stz(a_idx(w65.x, 1)); break;
    case 0xE0: if (X8) cmp8((u8)w65.x, fetch8()); else cmp16(w65.x, fetch16()); break;
    case 0xE4: op_cpx(a_dp()); break; case 0xEC: op_cpx(a_abs()); break;
    case 0xC0: if (X8) cmp8((u8)w65.y, fetch8()); else cmp16(w65.y, fetch16()); break;
    case 0xC4: op_cpy(a_dp()); break; case 0xCC: op_cpy(a_abs()); break;
    /* branches */
    case 0x10: branch(!(w65.p & NF)); break; case 0x30: branch(w65.p & NF); break;
    case 0x50: branch(!(w65.p & VF)); break; case 0x70: branch(w65.p & VF); break;
    case 0x80: branch(1); break;
    case 0x90: branch(!(w65.p & CF)); break; case 0xB0: branch(w65.p & CF); break;
    case 0xD0: branch(!(w65.p & ZF)); break; case 0xF0: branch(w65.p & ZF); break;
    case 0x82: { s16 o = (s16)fetch16(); IDLE(); w65.pc = (u16)(w65.pc + o); break; }
    /* jumps, calls */
    case 0x4C: w65.pc = fetch16(); break;
    case 0x5C: { u32 t = fetch24(); w65.pc = (u16)t; w65.pb = (u8)(t >> 16); break; }
    case 0x6C: { u16 p = fetch16(); w65.pc = rd16z(p); break; }
    case 0x7C: { u16 p = fetch16(); IDLE(); w65.pc = rd16((u32)w65.pb << 16 | (u16)(p + w65.x)); break; }
    case 0xDC: { u16 p = fetch16(); w65.pc = rd16z(p); w65.pb = RD((u16)(p + 2)); break; }
    case 0x20: { u16 t = fetch16(); IDLE(); push16((u16)(w65.pc - 1)); w65.pc = t; break; }
    case 0x22: { u16 lo = fetch16(); push8(w65.pb); IDLE(); u8 b = fetch8(); push16((u16)(w65.pc - 1)); w65.pc = lo; w65.pb = b; break; }
    case 0xFC: {
        u8 l = fetch8(); push8((u8)(w65.pc >> 8)); push8((u8)w65.pc);
        u8 h = fetch8(); IDLE();
        w65.pc = rd16((u32)w65.pb << 16 | (u16)((h << 8 | l) + w65.x));
        break; }
    case 0x60: IDLE(); IDLE(); w65.pc = (u16)(pop16() + 1); IDLE(); break;
    case 0x6B: IDLE(); IDLE(); { u16 t = pop16(); w65.pb = pop8(); w65.pc = (u16)(t + 1); } break;
    case 0x40:
        IDLE(); IDLE();
        w65.p = pop8(); fix_p();
        w65.pc = pop16();
        if (!w65.e) w65.pb = pop8();
        break;
    case 0x00: w65.pc++; interrupt(0xFFE6, 0xFFFE, 1); break;
    case 0x02: w65.pc++; interrupt(0xFFE4, 0xFFF4, 0); break;
    /* stack */
    case 0x48: IDLE(); if (M8) push8((u8)w65.a); else push16(w65.a); break;
    case 0x68: IDLE(); IDLE(); if (M8) { u8 v = pop8(); seta8(v); nz8(v); } else { w65.a = pop16(); nz16(w65.a); } break;
    case 0xDA: IDLE(); if (X8) push8((u8)w65.x); else push16(w65.x); break;
    case 0xFA: IDLE(); IDLE(); if (X8) { w65.x = pop8(); nz8((u8)w65.x); } else { w65.x = pop16(); nz16(w65.x); } break;
    case 0x5A: IDLE(); if (X8) push8((u8)w65.y); else push16(w65.y); break;
    case 0x7A: IDLE(); IDLE(); if (X8) { w65.y = pop8(); nz8((u8)w65.y); } else { w65.y = pop16(); nz16(w65.y); } break;
    case 0x08: IDLE(); push8(w65.p); break;
    case 0x28: IDLE(); IDLE(); w65.p = pop8(); fix_p(); break;
    case 0x8B: IDLE(); push8(w65.db); break;
    case 0xAB: IDLE(); IDLE(); w65.db = pop8(); nz8(w65.db); break;
    case 0x0B: IDLE(); push16(w65.d); break;
    case 0x2B: IDLE(); IDLE(); w65.d = pop16(); nz16(w65.d); break;
    case 0x4B: IDLE(); push8(w65.pb); break;
    case 0xF4: { u16 v = fetch16(); push16(v); break; }
    case 0xD4: { u16 p = (u16)a_dp(); push16(rd16z(p)); break; }
    case 0x62: { s16 o = (s16)fetch16(); IDLE(); push16((u16)(w65.pc + o)); break; }
    /* transfers */
    case 0xAA: IDLE(); if (X8) { w65.x = w65.a & 0xFF; nz8((u8)w65.x); } else { w65.x = w65.a; nz16(w65.x); } break;
    case 0xA8: IDLE(); if (X8) { w65.y = w65.a & 0xFF; nz8((u8)w65.y); } else { w65.y = w65.a; nz16(w65.y); } break;
    case 0x8A: IDLE(); if (M8) { seta8((u8)w65.x); nz8((u8)w65.x); } else { w65.a = w65.x; nz16(w65.a); } break;
    case 0x98: IDLE(); if (M8) { seta8((u8)w65.y); nz8((u8)w65.y); } else { w65.a = w65.y; nz16(w65.a); } break;
    case 0x9B: IDLE(); w65.y = w65.x; if (X8) nz8((u8)w65.y); else nz16(w65.y); break;
    case 0xBB: IDLE(); w65.x = w65.y; if (X8) nz8((u8)w65.x); else nz16(w65.x); break;
    case 0xBA: IDLE(); if (X8) { w65.x = w65.s & 0xFF; nz8((u8)w65.x); } else { w65.x = w65.s; nz16(w65.x); } break;
    case 0x9A: IDLE(); w65.s = w65.e ? (u16)(0x100 | (w65.x & 0xFF)) : w65.x; break;
    case 0x5B: IDLE(); w65.d = w65.a; nz16(w65.d); break;
    case 0x7B: IDLE(); w65.a = w65.d; nz16(w65.a); break;
    case 0x1B: IDLE(); w65.s = w65.e ? (u16)(0x100 | (w65.a & 0xFF)) : w65.a; break;
    case 0x3B: IDLE(); w65.a = w65.s; nz16(w65.a); break;
    case 0xEB: IDLE(); IDLE(); w65.a = (u16)(w65.a >> 8 | w65.a << 8); nz8((u8)w65.a); break;
    /* index inc/dec */
    case 0xE8: IDLE(); if (X8) { w65.x = (u8)(w65.x + 1); nz8((u8)w65.x); } else { w65.x++; nz16(w65.x); } break;
    case 0xCA: IDLE(); if (X8) { w65.x = (u8)(w65.x - 1); nz8((u8)w65.x); } else { w65.x--; nz16(w65.x); } break;
    case 0xC8: IDLE(); if (X8) { w65.y = (u8)(w65.y + 1); nz8((u8)w65.y); } else { w65.y++; nz16(w65.y); } break;
    case 0x88: IDLE(); if (X8) { w65.y = (u8)(w65.y - 1); nz8((u8)w65.y); } else { w65.y--; nz16(w65.y); } break;
    /* flags */
    case 0x18: IDLE(); w65.p &= ~CF; break; case 0x38: IDLE(); w65.p |= CF; break;
    case 0x58: IDLE(); w65.p &= ~IF; break; case 0x78: IDLE(); w65.p |= IF; break;
    case 0xB8: IDLE(); w65.p &= ~VF; break;
    case 0xD8: IDLE(); w65.p &= ~DF; break; case 0xF8: IDLE(); w65.p |= DF; break;
    case 0xC2: { u8 v = fetch8(); IDLE(); w65.p &= (u8)~v; fix_p(); break; }
    case 0xE2: { u8 v = fetch8(); IDLE(); w65.p |= v; fix_p(); break; }
    case 0xFB: {
        IDLE();
        u8 c = w65.p & CF;
        w65.p = (u8)((w65.p & ~CF) | w65.e);
        w65.e = c;
        if (w65.e) { w65.s = (u16)(0x100 | (w65.s & 0xFF)); }
        fix_p();
        break; }
    /* block moves */
    case 0x54: case 0x44: {
        u8 dst = fetch8(), src = fetch8();
        w65.db = dst;
        u8 v = RD((u32)src << 16 | w65.x);
        WR((u32)dst << 16 | w65.y, v);
        IDLE(); IDLE();
        int d = op == 0x54 ? 1 : -1;
        w65.x = (u16)(w65.x + d); w65.y = (u16)(w65.y + d);
        if (X8) { w65.x &= 0xFF; w65.y &= 0xFF; }
        if (w65.a--) w65.pc = (u16)(w65.pc - 3);
        break; }
    /* misc */
    case 0xEA: IDLE(); break;
    case 0x42: fetch8(); break;
    case 0xCB: IDLE(); IDLE(); w65.waiting = 1; break;
    case 0xDB: IDLE(); IDLE(); w65.stopped = 1; break;
    default: break;   /* every opcode is handled above */
    }
    (void)a;
}

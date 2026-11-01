/*
 * z80.c - Zilog Z80 interpreter for the Mega Drive sound CPU.
 *
 * Implements all documented opcodes plus the commonly used undocumented
 * ones (IXH/IXL/IYH/IYL, SLL, DDCB/FDCB register copies, ED mirrors,
 * IN F,(C) / OUT (C),0) with S Z H P/V N C flags and the undocumented
 * X/Y (bits 3/5) flags, a simplified MEMPTR (WZ) for BIT n,(HL),
 * per-instruction T-state counts, the one-instruction EI delay, HALT,
 * IM 0/1/2, NMI and R register increments on every M1 cycle.
 */
#include "genesis.h"

typedef union { u16 w; struct { u8 l, h; } b; } Pair;

EMU_STATE static Pair AF, BC, DE, HL, IX, IY, SP, PC, WZ;
EMU_STATE static Pair AF2, BC2, DE2, HL2;
EMU_STATE static u8 I, R, R7;                 /* R7: bit 7 of R as set by LD R,A */
EMU_STATE static u8 IFF1, IFF2, IM, HALTED, EI_DELAY, IRQ_LINE, NMI_PENDING;
EMU_STATE static int cyc;                     /* T-states of the current step */
EMU_STATE static int idx;                     /* 0 = HL, 1 = IX, 2 = IY */
EMU_STATE static Pair *XY;

#define rA AF.b.h
#define rF AF.b.l
#define rB BC.b.h
#define rC BC.b.l
#define rD DE.b.h
#define rE DE.b.l
#define rH HL.b.h
#define rL HL.b.l

#define CF 0x01
#define NF 0x02
#define PF 0x04
#define XF 0x08
#define HF 0x10
#define YF 0x20
#define ZF 0x40
#define SF 0x80

static u8 SZ[256], SZP[256];
static int tables_ready;

static void init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        SZ[i] = (u8)((i ? (i & SF) : ZF) | (i & (XF | YF)));
        SZP[i] = (u8)(SZ[i] | (p ? 0 : PF));
    }
    tables_ready = 1;
}

/* ------------------------------------------------------------ memory */
static inline u8 rd(u16 a) { return z80_mem_read(a); }
static inline void wr(u16 a, u8 v) { z80_mem_write(a, v); }
static inline u8 fetch(void) { return rd(PC.w++); }
static inline u16 fetch16(void) { u16 l = fetch(); return (u16)(l | (fetch() << 8)); }
static inline u16 rd16(u16 a) { u16 l = rd(a); return (u16)(l | (rd((u16)(a + 1)) << 8)); }
static inline void wr16(u16 a, u16 v) { wr(a, (u8)v); wr((u16)(a + 1), (u8)(v >> 8)); }
static inline void push(u16 v) { SP.w--; wr(SP.w, (u8)(v >> 8)); SP.w--; wr(SP.w, (u8)v); }
static inline u16 pop(void) { u16 v = rd16(SP.w); SP.w += 2; return v; }
static inline void incr(void) { R = (u8)((R + 1) & 0x7f); }

/* ------------------------------------------------------------ registers */
/* r: 0 B 1 C 2 D 3 E 4 H 5 L 7 A (6 = (HL), handled by callers) */
static inline u8 get_r(int r)   /* H/L substituted by IXH/IXL etc. */
{
    switch (r) {
    case 0: return rB; case 1: return rC; case 2: return rD; case 3: return rE;
    case 4: return XY->b.h; case 5: return XY->b.l; default: return rA;
    }
}
static inline void set_r(int r, u8 v)
{
    switch (r) {
    case 0: rB = v; break; case 1: rC = v; break; case 2: rD = v; break;
    case 3: rE = v; break; case 4: XY->b.h = v; break; case 5: XY->b.l = v; break;
    default: rA = v; break;
    }
}
static inline u8 get_r0(int r)  /* real H/L */
{
    switch (r) {
    case 0: return rB; case 1: return rC; case 2: return rD; case 3: return rE;
    case 4: return rH; case 5: return rL; default: return rA;
    }
}
static inline void set_r0(int r, u8 v)
{
    switch (r) {
    case 0: rB = v; break; case 1: rC = v; break; case 2: rD = v; break;
    case 3: rE = v; break; case 4: rH = v; break; case 5: rL = v; break;
    default: rA = v; break;
    }
}

/* effective address of (HL) / (IX+d) / (IY+d) */
static inline u16 ea(void)
{
    if (!idx) return HL.w;
    s8 d = (s8)fetch();
    u16 a = (u16)(XY->w + d);
    WZ.w = a;
    cyc += 8;
    return a;
}

/* ------------------------------------------------------------ ALU */
static inline void add8(u8 v, int c)
{
    unsigned r = (unsigned)rA + v + (unsigned)c;
    rF = (u8)(SZ[r & 0xff] | ((r >> 8) & CF) | ((rA ^ v ^ r) & HF) |
              (((rA ^ ~v) & (rA ^ r) & 0x80) >> 5));
    rA = (u8)r;
}
static inline u8 sub8f(u8 v, int c)
{
    unsigned r = (unsigned)rA - v - (unsigned)c;
    rF = (u8)(SZ[r & 0xff] | ((r >> 8) & CF) | NF | ((rA ^ v ^ r) & HF) |
              (((rA ^ v) & (rA ^ r) & 0x80) >> 5));
    return (u8)r;
}
static inline void alu(int n, u8 v)
{
    switch (n) {
    case 0: add8(v, 0); break;
    case 1: add8(v, rF & CF); break;
    case 2: rA = sub8f(v, 0); break;
    case 3: rA = sub8f(v, rF & CF); break;
    case 4: rA &= v; rF = (u8)(SZP[rA] | HF); break;
    case 5: rA ^= v; rF = SZP[rA]; break;
    case 6: rA |= v; rF = SZP[rA]; break;
    default: sub8f(v, 0); rF = (u8)((rF & ~(XF | YF)) | (v & (XF | YF))); break;
    }
}
static inline u8 inc8(u8 v)
{
    u8 r = (u8)(v + 1);
    rF = (u8)((rF & CF) | SZ[r] | (r == 0x80 ? PF : 0) | ((r & 0x0f) == 0 ? HF : 0));
    return r;
}
static inline u8 dec8(u8 v)
{
    u8 r = (u8)(v - 1);
    rF = (u8)((rF & CF) | NF | SZ[r] | (r == 0x7f ? PF : 0) | ((r & 0x0f) == 0x0f ? HF : 0));
    return r;
}
static inline u16 add16(u16 a, u16 b)
{
    u32 r = (u32)a + b;
    rF = (u8)((rF & (SF | ZF | PF)) | (((a ^ b ^ r) >> 8) & HF) | ((r >> 16) & CF) |
              ((r >> 8) & (XF | YF)));
    WZ.w = (u16)(a + 1);
    return (u16)r;
}
static inline void adc16(u16 v)
{
    u16 a = HL.w;
    u32 r = (u32)a + v + (rF & CF);
    rF = (u8)(((r >> 8) & (SF | XF | YF)) | ((r & 0xffff) ? 0 : ZF) |
              (((a ^ v ^ r) >> 8) & HF) | ((r >> 16) & CF) |
              ((~(a ^ v) & (a ^ r) & 0x8000) >> 13));
    WZ.w = (u16)(a + 1);
    HL.w = (u16)r;
}
static inline void sbc16(u16 v)
{
    u16 a = HL.w;
    u32 r = (u32)a - v - (rF & CF);
    rF = (u8)(NF | ((r >> 8) & (SF | XF | YF)) | ((r & 0xffff) ? 0 : ZF) |
              (((a ^ v ^ r) >> 8) & HF) | ((r >> 16) & CF) |
              (((a ^ v) & (a ^ r) & 0x8000) >> 13));
    WZ.w = (u16)(a + 1);
    HL.w = (u16)r;
}
static inline void daa(void)
{
    u8 a = rA, d = 0, c = 0, h;
    if ((rF & HF) || (a & 0x0f) > 9) d |= 0x06;
    if ((rF & CF) || a > 0x99) { d |= 0x60; c = CF; }
    if (rF & NF) { h = (u8)(((rF & HF) && (a & 0x0f) < 6) ? HF : 0); a = (u8)(a - d); }
    else { h = (u8)(((a & 0x0f) > 9) ? HF : 0); a = (u8)(a + d); }
    rA = a;
    rF = (u8)(SZP[a] | c | (rF & NF) | h);
}
static inline u8 rot(int n, u8 v)
{
    u8 r, c;
    switch (n) {
    case 0: c = v >> 7; r = (u8)((v << 1) | c); break;            /* RLC */
    case 1: c = v & 1; r = (u8)((v >> 1) | (c << 7)); break;       /* RRC */
    case 2: c = v >> 7; r = (u8)((v << 1) | (rF & CF)); break;     /* RL  */
    case 3: c = v & 1; r = (u8)((v >> 1) | ((rF & CF) << 7)); break; /* RR */
    case 4: c = v >> 7; r = (u8)(v << 1); break;                   /* SLA */
    case 5: c = v & 1; r = (u8)((v >> 1) | (v & 0x80)); break;     /* SRA */
    case 6: c = v >> 7; r = (u8)((v << 1) | 1); break;             /* SLL */
    default: c = v & 1; r = (u8)(v >> 1); break;                   /* SRL */
    }
    rF = (u8)(SZP[r] | c);
    return r;
}
static inline void bit_op(int b, u8 v, u8 xy)
{
    u8 r = (u8)(v & (1 << b));
    rF = (u8)((rF & CF) | HF | (r ? (r & SF) : (ZF | PF)) | (xy & (XF | YF)));
}
static inline int cond(int n)
{
    switch (n) {
    case 0: return !(rF & ZF); case 1: return rF & ZF;
    case 2: return !(rF & CF); case 3: return rF & CF;
    case 4: return !(rF & PF); case 5: return rF & PF;
    case 6: return !(rF & SF); default: return rF & SF;
    }
}

/* ------------------------------------------------------------ CB */
static void exec_cb(void)
{
    u8 op = fetch();
    incr();
    int r = op & 7, n = (op >> 3) & 7;
    if (r == 6) {
        u8 v = rd(HL.w);
        if (op < 0x40)      { wr(HL.w, rot(n, v)); cyc += 15; }
        else if (op < 0x80) { bit_op(n, v, WZ.b.h); cyc += 12; }
        else if (op < 0xC0) { wr(HL.w, (u8)(v & ~(1 << n))); cyc += 15; }
        else                { wr(HL.w, (u8)(v | (1 << n))); cyc += 15; }
    } else {
        u8 v = get_r0(r);
        if (op < 0x40)      set_r0(r, rot(n, v));
        else if (op < 0x80) bit_op(n, v, v);
        else if (op < 0xC0) set_r0(r, (u8)(v & ~(1 << n)));
        else                set_r0(r, (u8)(v | (1 << n)));
        cyc += 8;
    }
}

/* DDCB / FDCB: prefix already accounted (4 T) */
static void exec_xycb(void)
{
    s8 d = (s8)fetch();
    u8 op = fetch();
    u16 a = (u16)(XY->w + d);
    WZ.w = a;
    int r = op & 7, n = (op >> 3) & 7;
    u8 v = rd(a), res;
    if ((op & 0xC0) == 0x40) { bit_op(n, v, WZ.b.h); cyc += 16; return; }
    if (op < 0x40) res = rot(n, v);
    else if (op < 0xC0) res = (u8)(v & ~(1 << n));
    else res = (u8)(v | (1 << n));
    wr(a, res);
    if (r != 6) set_r0(r, res);     /* undocumented register copy */
    cyc += 19;
}

/* ------------------------------------------------------------ ED */
static inline void block_io_flags(u8 v, unsigned k)
{
    rF = (u8)(SZ[rB] | ((v & 0x80) ? NF : 0) | (k > 0xff ? (HF | CF) : 0) |
              (SZP[(k & 7) ^ rB] & PF));
}

static void exec_ed(void)
{
    u8 op = fetch();
    incr();
    Pair *rr;
    switch ((op >> 4) & 3) {
    case 0: rr = &BC; break; case 1: rr = &DE; break; case 2: rr = &HL; break; default: rr = &SP; break;
    }
    if (op >= 0x40 && op < 0x80) {
        int r = (op >> 3) & 7;
        switch (op & 7) {
        case 0: { /* IN r,(C) */
            u8 v = z80_io_read(BC.w);
            WZ.w = (u16)(BC.w + 1);
            if (r != 6) set_r0(r, v);
            rF = (u8)((rF & CF) | SZP[v]);
            cyc += 12; return;
        }
        case 1: /* OUT (C),r */
            z80_io_write(BC.w, r == 6 ? 0 : get_r0(r));
            WZ.w = (u16)(BC.w + 1);
            cyc += 12; return;
        case 2:
            if (op & 8) adc16(rr->w); else sbc16(rr->w);
            cyc += 15; return;
        case 3: {
            u16 a = fetch16();
            if (op & 8) rr->w = rd16(a); else wr16(a, rr->w);
            WZ.w = (u16)(a + 1);
            cyc += 20; return;
        }
        case 4: { /* NEG */
            u8 v = rA; rA = 0; rA = sub8f(v, 0);
            cyc += 8; return;
        }
        case 5: /* RETN / RETI */
            IFF1 = IFF2;
            PC.w = pop(); WZ = PC;
            cyc += 14; return;
        case 6: {
            static const u8 ims[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
            IM = ims[r];
            cyc += 8; return;
        }
        default:
            switch (r) {
            case 0: I = rA; cyc += 9; return;                       /* LD I,A */
            case 1: R = rA & 0x7f; R7 = rA & 0x80; cyc += 9; return; /* LD R,A */
            case 2: rA = I;                                           /* LD A,I */
                rF = (u8)((rF & CF) | SZ[rA] | (IFF2 ? PF : 0)); cyc += 9; return;
            case 3: rA = (u8)((R & 0x7f) | R7);                      /* LD A,R */
                rF = (u8)((rF & CF) | SZ[rA] | (IFF2 ? PF : 0)); cyc += 9; return;
            case 4: { /* RRD */
                u8 m = rd(HL.w);
                wr(HL.w, (u8)((rA << 4) | (m >> 4)));
                rA = (u8)((rA & 0xf0) | (m & 0x0f));
                rF = (u8)((rF & CF) | SZP[rA]);
                WZ.w = (u16)(HL.w + 1);
                cyc += 18; return;
            }
            case 5: { /* RLD */
                u8 m = rd(HL.w);
                wr(HL.w, (u8)((m << 4) | (rA & 0x0f)));
                rA = (u8)((rA & 0xf0) | (m >> 4));
                rF = (u8)((rF & CF) | SZP[rA]);
                WZ.w = (u16)(HL.w + 1);
                cyc += 18; return;
            }
            default: cyc += 8; return;                              /* ED 77/7F: NOP */
            }
        }
    }
    if (op >= 0xA0 && op <= 0xBB && (op & 7) <= 3 && (op & 0x0f) != 4) {
        int dec = (op & 8) != 0, rep = (op & 0x10) != 0;
        int delta = dec ? -1 : 1;
        cyc += 16;
        switch (op & 3) {
        case 0: { /* LDI/LDD */
            u8 v = rd(HL.w);
            wr(DE.w, v);
            HL.w = (u16)(HL.w + delta); DE.w = (u16)(DE.w + delta); BC.w--;
            u8 n = (u8)(v + rA);
            rF = (u8)((rF & (SF | ZF | CF)) | (BC.w ? PF : 0) | (n & XF) | ((n & 0x02) << 4));
            if (rep && BC.w) { PC.w -= 2; WZ.w = (u16)(PC.w + 1); cyc += 5; }
            return;
        }
        case 1: { /* CPI/CPD */
            u8 v = rd(HL.w);
            u8 r = (u8)(rA - v);
            u8 h = (u8)((rA ^ v ^ r) & HF);
            HL.w = (u16)(HL.w + delta); BC.w--;
            WZ.w = (u16)(WZ.w + delta);
            u8 n = (u8)(r - (h ? 1 : 0));
            rF = (u8)((rF & CF) | NF | (SZ[r] & (SF | ZF)) | h | (BC.w ? PF : 0) |
                      (n & XF) | ((n & 0x02) << 4));
            if (rep && BC.w && !(rF & ZF)) { PC.w -= 2; WZ.w = (u16)(PC.w + 1); cyc += 5; }
            return;
        }
        case 2: { /* INI/IND */
            u8 v = z80_io_read(BC.w);
            WZ.w = (u16)(BC.w + delta);
            wr(HL.w, v);
            rB--;
            HL.w = (u16)(HL.w + delta);
            block_io_flags(v, (unsigned)v + (u8)(rC + delta));
            if (rep && rB) { PC.w -= 2; cyc += 5; }
            return;
        }
        default: { /* OUTI/OUTD */
            u8 v = rd(HL.w);
            rB--;
            WZ.w = (u16)(BC.w + delta);
            z80_io_write(BC.w, v);
            HL.w = (u16)(HL.w + delta);
            block_io_flags(v, (unsigned)v + rL);
            if (rep && rB) { PC.w -= 2; cyc += 5; }
            return;
        }
        }
    }
    cyc += 8; /* undefined ED xx: 8 T NOP */
}

/* ------------------------------------------------------------ main */
static void exec_main(u8 op)
{
    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) { HALTED = 1; cyc += 4; return; }
        int d = (op >> 3) & 7, s = op & 7;
        if (s == 6)      { u16 a = ea(); set_r0(d, rd(a)); cyc += 7; }
        else if (d == 6) { u16 a = ea(); wr(a, get_r0(s)); cyc += 7; }
        else             { set_r(d, get_r(s)); cyc += 4; }
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        int s = op & 7;
        u8 v;
        if (s == 6) { v = rd(ea()); cyc += 7; } else { v = get_r(s); cyc += 4; }
        alu((op >> 3) & 7, v);
        return;
    }
    if (op < 0x40) {
        int r = (op >> 3) & 7;
        switch (op & 7) {
        case 4:
            if (r == 6) { u16 a = ea(); wr(a, inc8(rd(a))); cyc += 11; }
            else { set_r(r, inc8(get_r(r))); cyc += 4; }
            return;
        case 5:
            if (r == 6) { u16 a = ea(); wr(a, dec8(rd(a))); cyc += 11; }
            else { set_r(r, dec8(get_r(r))); cyc += 4; }
            return;
        case 6:
            if (r == 6) {
                u16 a = ea();
                if (idx) cyc -= 3;   /* LD (IX+d),n = 19 */
                wr(a, fetch()); cyc += 10;
            } else { set_r(r, fetch()); cyc += 7; }
            return;
        default: break;
        }
    }
    if (op >= 0xC0) {
        int n = (op >> 3) & 7;
        switch (op & 7) {
        case 0: /* RET cc */
            if (cond(n)) { PC.w = pop(); WZ = PC; cyc += 11; } else cyc += 5;
            return;
        case 2: { /* JP cc,nn */
            u16 a = fetch16(); WZ.w = a;
            if (cond(n)) PC.w = a;
            cyc += 10; return;
        }
        case 4: { /* CALL cc,nn */
            u16 a = fetch16(); WZ.w = a;
            if (cond(n)) { push(PC.w); PC.w = a; cyc += 17; } else cyc += 10;
            return;
        }
        case 6: /* ALU n */
            alu(n, fetch()); cyc += 7; return;
        case 7: /* RST */
            push(PC.w); PC.w = (u16)(op & 0x38); WZ = PC; cyc += 11; return;
        default: break;
        }
    }

    switch (op) {
    case 0x00: cyc += 4; break;
    case 0x01: BC.w = fetch16(); cyc += 10; break;
    case 0x11: DE.w = fetch16(); cyc += 10; break;
    case 0x21: XY->w = fetch16(); cyc += 10; break;
    case 0x31: SP.w = fetch16(); cyc += 10; break;
    case 0x02: wr(BC.w, rA); WZ.b.l = (u8)(BC.w + 1); WZ.b.h = rA; cyc += 7; break;
    case 0x12: wr(DE.w, rA); WZ.b.l = (u8)(DE.w + 1); WZ.b.h = rA; cyc += 7; break;
    case 0x0A: rA = rd(BC.w); WZ.w = (u16)(BC.w + 1); cyc += 7; break;
    case 0x1A: rA = rd(DE.w); WZ.w = (u16)(DE.w + 1); cyc += 7; break;
    case 0x03: BC.w++; cyc += 6; break;
    case 0x13: DE.w++; cyc += 6; break;
    case 0x23: XY->w++; cyc += 6; break;
    case 0x33: SP.w++; cyc += 6; break;
    case 0x0B: BC.w--; cyc += 6; break;
    case 0x1B: DE.w--; cyc += 6; break;
    case 0x2B: XY->w--; cyc += 6; break;
    case 0x3B: SP.w--; cyc += 6; break;
    case 0x09: XY->w = add16(XY->w, BC.w); cyc += 11; break;
    case 0x19: XY->w = add16(XY->w, DE.w); cyc += 11; break;
    case 0x29: XY->w = add16(XY->w, XY->w); cyc += 11; break;
    case 0x39: XY->w = add16(XY->w, SP.w); cyc += 11; break;
    case 0x07: rA = (u8)((rA << 1) | (rA >> 7));
        rF = (u8)((rF & (SF | ZF | PF)) | (rA & (XF | YF | CF))); cyc += 4; break;
    case 0x0F: { u8 c = rA & 1; rA = (u8)((rA >> 1) | (rA << 7));
        rF = (u8)((rF & (SF | ZF | PF)) | c | (rA & (XF | YF))); cyc += 4; break; }
    case 0x17: { u8 c = rA >> 7; rA = (u8)((rA << 1) | (rF & CF));
        rF = (u8)((rF & (SF | ZF | PF)) | c | (rA & (XF | YF))); cyc += 4; break; }
    case 0x1F: { u8 c = rA & 1; rA = (u8)((rA >> 1) | ((rF & CF) << 7));
        rF = (u8)((rF & (SF | ZF | PF)) | c | (rA & (XF | YF))); cyc += 4; break; }
    case 0x08: { Pair t = AF; AF = AF2; AF2 = t; cyc += 4; break; }
    case 0x10: { s8 d = (s8)fetch();
        if (--rB) { PC.w = (u16)(PC.w + d); WZ = PC; cyc += 13; } else cyc += 8; break; }
    case 0x18: { s8 d = (s8)fetch(); PC.w = (u16)(PC.w + d); WZ = PC; cyc += 12; break; }
    case 0x20: case 0x28: case 0x30: case 0x38: { s8 d = (s8)fetch();
        if (cond((op >> 3) & 3)) { PC.w = (u16)(PC.w + d); WZ = PC; cyc += 12; } else cyc += 7; break; }
    case 0x22: { u16 a = fetch16(); wr16(a, XY->w); WZ.w = (u16)(a + 1); cyc += 16; break; }
    case 0x2A: { u16 a = fetch16(); XY->w = rd16(a); WZ.w = (u16)(a + 1); cyc += 16; break; }
    case 0x32: { u16 a = fetch16(); wr(a, rA); WZ.b.l = (u8)(a + 1); WZ.b.h = rA; cyc += 13; break; }
    case 0x3A: { u16 a = fetch16(); rA = rd(a); WZ.w = (u16)(a + 1); cyc += 13; break; }
    case 0x27: daa(); cyc += 4; break;
    case 0x2F: rA = (u8)~rA; rF = (u8)((rF & (SF | ZF | PF | CF)) | HF | NF | (rA & (XF | YF))); cyc += 4; break;
    case 0x37: rF = (u8)((rF & (SF | ZF | PF)) | CF | (rA & (XF | YF))); cyc += 4; break;
    case 0x3F: rF = (u8)(((rF & (SF | ZF | PF | CF)) | ((rF & CF) << 4) | (rA & (XF | YF))) ^ CF); cyc += 4; break;

    case 0xC1: BC.w = pop(); cyc += 10; break;
    case 0xD1: DE.w = pop(); cyc += 10; break;
    case 0xE1: XY->w = pop(); cyc += 10; break;
    case 0xF1: AF.w = pop(); cyc += 10; break;
    case 0xC5: push(BC.w); cyc += 11; break;
    case 0xD5: push(DE.w); cyc += 11; break;
    case 0xE5: push(XY->w); cyc += 11; break;
    case 0xF5: push(AF.w); cyc += 11; break;
    case 0xC3: PC.w = fetch16(); WZ = PC; cyc += 10; break;
    case 0xC9: PC.w = pop(); WZ = PC; cyc += 10; break;
    case 0xCD: { u16 a = fetch16(); push(PC.w); PC.w = a; WZ = PC; cyc += 17; break; }
    case 0xD3: { u8 n = fetch(); z80_io_write((u16)(n | (rA << 8)), rA);
        WZ.b.l = (u8)(n + 1); WZ.b.h = rA; cyc += 11; break; }
    case 0xDB: { u8 n = fetch(); u16 p = (u16)(n | (rA << 8)); rA = z80_io_read(p);
        WZ.w = (u16)(p + 1); cyc += 11; break; }
    case 0xD9: { Pair t;
        t = BC; BC = BC2; BC2 = t; t = DE; DE = DE2; DE2 = t; t = HL; HL = HL2; HL2 = t;
        cyc += 4; break; }
    case 0xE3: { u16 v = rd16(SP.w); wr16(SP.w, XY->w); XY->w = v; WZ.w = v; cyc += 19; break; }
    case 0xE9: PC.w = XY->w; cyc += 4; break;
    case 0xEB: { u16 t = DE.w; DE.w = HL.w; HL.w = t; cyc += 4; break; }
    case 0xF3: IFF1 = IFF2 = 0; cyc += 4; break;
    case 0xFB: IFF1 = IFF2 = 1; EI_DELAY = 1; cyc += 4; break;
    case 0xF9: SP.w = XY->w; cyc += 6; break;
    case 0xCB:
        if (idx) exec_xycb(); else exec_cb();
        break;
    case 0xED: exec_ed(); break;
    default: cyc += 4; break; /* unreachable */
    }
}

static void step(void)
{
    idx = 0;
    XY = &HL;
    u8 op = fetch();
    incr();
    while (op == 0xDD || op == 0xFD) {
        idx = (op == 0xDD) ? 1 : 2;
        XY = (idx == 1) ? &IX : &IY;
        cyc += 4;
        op = fetch();
        incr();
    }
    if (op == 0xED) idx = 0, XY = &HL;
    exec_main(op);
}

static int take_irq(void)
{
    HALTED = 0;
    IFF1 = IFF2 = 0;
    incr();
    push(PC.w);
    if (IM == 2) {
        PC.w = rd16((u16)((I << 8) | 0xFF));
        WZ = PC;
        return 19;
    }
    /* IM 1, and IM 0 with 0xFF (RST 38h) on the data bus */
    PC.w = 0x38;
    WZ = PC;
    return 13;
}

/* ------------------------------------------------------------ API */
void z80_reset(void)
{
    if (!tables_ready) init_tables();
    AF.w = BC.w = DE.w = HL.w = IX.w = IY.w = 0xFFFF;
    AF2.w = BC2.w = DE2.w = HL2.w = 0xFFFF;
    SP.w = 0xFFFF;
    PC.w = 0;
    WZ.w = 0;
    I = R = R7 = 0;
    IFF1 = IFF2 = 0;
    IM = 0;
    HALTED = EI_DELAY = 0;
    NMI_PENDING = 0;
}

void z80_set_irq(int state) { IRQ_LINE = state ? 1 : 0; }
void z80_nmi(void) { NMI_PENDING = 1; }

int z80_execute(int cycles)
{
    int done = 0;
    if (!tables_ready) init_tables();
    while (done < cycles) {
        if (NMI_PENDING) {
            NMI_PENDING = 0;
            HALTED = 0;
            IFF1 = 0;
            EI_DELAY = 0;
            incr();
            push(PC.w);
            PC.w = 0x66;
            done += 11;
            continue;
        }
        if (IRQ_LINE && IFF1 && !EI_DELAY) {
            done += take_irq();
            continue;
        }
        EI_DELAY = 0;
        if (HALTED) {
            /* execute NOPs until the end of the slice */
            int n = (cycles - done + 3) / 4;
            R = (u8)((R + n) & 0x7f);
            done += n * 4;
            continue;
        }
        cyc = 0;
        step();
        done += cyc;
    }
    return done;
}

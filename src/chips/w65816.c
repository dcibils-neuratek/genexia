/*
 * w65816.c - WDC 65C816 interpreter. See w65816.h.
 * Every bus access and internal cycle goes through the machine's callbacks,
 * which is where memory speed and timing are accounted for.
 */
#include "w65816_ops.h"

EMU_STATE W65816 w65;

#define a_dp()  a_dp_v(fetch8())
#define a_dpx() a_dpx_v(fetch8())
#define a_dpy() a_dpy_v(fetch8())
#define a_abs() a_abs_v(fetch16())
#define a_idx(i, w) a_idx_v(fetch16(), (i), (w))
#define a_abl() fetch24()
#define a_alx() a_alx_v(fetch24())
#define a_ind() a_ind_v(fetch8())
#define a_inx() a_inx_v(fetch8())
#define a_iny(w) a_iny_v(fetch8(), (w))
#define a_inl() a_inl_v(fetch8())
#define a_ily() a_ily_v(fetch8())
#define a_sr()  a_sr_v(fetch8())
#define a_sry() a_sry_v(fetch8())

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
    w65_exec();
}

/* one instruction, no interrupt checks: also called from recompiled blocks */
void w65_exec(void) {
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
        if (M8) op_bit_imm8(fetch8()); else op_bit_imm16(fetch16());
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
    case 0x60: op_rts(); break;
    case 0x6B: op_rtl(); break;
    case 0x40:
        IDLE(); IDLE();
        w65.p = pop8(); fix_p();
        w65.pc = pop16();
        if (!w65.e) w65.pb = pop8();
        break;
    case 0x00: w65.pc++; interrupt(0xFFE6, 0xFFFE, 1); break;
    case 0x02: w65.pc++; interrupt(0xFFE4, 0xFFF4, 0); break;
    /* stack */
    case 0x48: op_pha_(M8); break;
    case 0x68: op_pla_(M8); break;
    case 0xDA: op_phx_(X8); break;
    case 0xFA: op_plx_(X8); break;
    case 0x5A: op_phy_(X8); break;
    case 0x7A: op_ply_(X8); break;
    case 0x08: op_php(); break;
    case 0x28: IDLE(); IDLE(); w65.p = pop8(); fix_p(); break;
    case 0x8B: op_phb(); break;
    case 0xAB: op_plb(); break;
    case 0x0B: op_phd(); break;
    case 0x2B: op_pld(); break;
    case 0x4B: op_phk(); break;
    case 0xF4: { u16 v = fetch16(); push16(v); break; }
    case 0xD4: op_pei(fetch8()); break;
    case 0x62: { s16 o = (s16)fetch16(); IDLE(); push16((u16)(w65.pc + o)); break; }
    /* transfers */
    case 0xAA: op_tax_(X8); break;
    case 0xA8: op_tay_(X8); break;
    case 0x8A: op_txa_(M8); break;
    case 0x98: op_tya_(M8); break;
    case 0x9B: op_txy_(X8); break;
    case 0xBB: op_tyx_(X8); break;
    case 0xBA: op_tsx_(X8); break;
    case 0x9A: op_txs(); break;
    case 0x5B: op_tcd(); break;
    case 0x7B: op_tdc(); break;
    case 0x1B: op_tcs(); break;
    case 0x3B: op_tsc(); break;
    case 0xEB: op_xba(); break;
    /* index inc/dec */
    case 0xE8: op_inx_(X8); break;
    case 0xCA: op_dex_(X8); break;
    case 0xC8: op_iny_(X8); break;
    case 0x88: op_dey_(X8); break;
    /* flags */
    case 0x18: IDLE(); w65.p &= ~CF; break; case 0x38: IDLE(); w65.p |= CF; break;
    case 0x58: IDLE(); w65.p &= ~IF; break; case 0x78: IDLE(); w65.p |= IF; break;
    case 0xB8: IDLE(); w65.p &= ~VF; break;
    case 0xD8: IDLE(); w65.p &= ~DF; break; case 0xF8: IDLE(); w65.p |= DF; break;
    case 0xC2: op_rep(fetch8()); break;
    case 0xE2: op_sep(fetch8()); break;
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

/*
 * apu.c - SNES audio unit: SPC700 CPU, 64 KB APU RAM, timers, I/O ports,
 * boot ROM and S-DSP, written from the documented behaviour of the hardware.
 *
 * Timing model: instruction-granular. Each SPC700 instruction runs atomically,
 * then its cycles clock the timers and the DSP. The DSP produces one complete
 * stereo sample every 32 SPC cycles (it is not stepped clock by clock), which
 * is more than enough for the port handshakes games depend on.
 */
#include <string.h>
#include "snes.h"

/* ---------------------------------------------------------------- state */
#define RING_FRAMES 4096               /* about 7 video frames at 32 kHz */

typedef struct {
    s16 buf[12];                       /* decoded BRR samples, circular */
    s32 bufpos;                        /* oldest sample; moves 4 per decode */
    s32 interp;                        /* 12.12-ish position, 0..0x7FFF */
    s32 brr_off;                       /* byte in block, 1..7 */
    s32 kon_delay;
    s32 env_mode;
    s32 env, hidden_env;
    u16 brr_addr;
} Voice;

enum { ENV_RELEASE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN };

EMU_STATE static u8  aram[0x10000];
/* SPC700 */
EMU_STATE static u8  r_a, r_x, r_y, r_sp, r_psw;
EMU_STATE static u16 r_pc;
EMU_STATE static u8  smp_halted;
/* I/O */
EMU_STATE static u8  ipl_on, dsp_addr, ctrl;
EMU_STATE static u8  port_in[4], port_out[4];
EMU_STATE static u8  tmr_target[3], tmr_stage[3], tmr_out[3];
EMU_STATE static s32 tmr_div_slow, tmr_div_fast, dsp_div;
/* clock conversion */
EMU_STATE static s32 master_last, cycle_budget;
EMU_STATE static u32 master_frac;
/* S-DSP */
EMU_STATE static u8  dreg[128];
EMU_STATE static Voice voice[8];
EMU_STATE static u8  new_kon, kon_latch, koff_latch, every_other;
EMU_STATE static s32 env_counter, noise_lfsr;
EMU_STATE static s32 echo_offset, echo_length, echo_hist_pos;
EMU_STATE static s16 echo_hist[8][2];
EMU_STATE static u8  esa_latch;
/* sample ring: read position + count */
EMU_STATE static s16 ring[RING_FRAMES * 2];
EMU_STATE static s32 ring_rd, ring_count;

enum { F_C = 1, F_Z = 2, F_I = 4, F_H = 8, F_B = 16, F_P = 32, F_V = 64, F_N = 128 };

/* Boot ROM, an original 64-byte program speaking the standard upload protocol.
 *
 * FFC0 mov x,#$EF / mov sp,x / mov a,#0
 * FFC5 clr:  mov (x),a / dec x / bne clr      ; clears $01-$EF
 * FFC9 mov a,#$AA / mov y,#$BB / movw $F4,ya  ; announce ready
 * FFCF mov a,#$CC
 * FFD1 wait: cbne $F4,wait                    ; CPU kicks with $CC
 * FFD4 block: movw ya,$F6 / movw $00,ya       ; destination from ports 2,3
 * FFD8 movw ya,$F4 / mov $F4,a                ; echo the kick
 * FFDC mov a,y / mov x,a / bne recv           ; port 1 != 0: more data
 * FFE0 jmp [$0000+x]                          ; A = X = Y = 0 on entry
 * FFE3 recv: mov y,$F4 / bne recv             ; index 0 starts the block
 * FFE7 data: cmp y,$F4 / bne check
 * FFEB mov a,$F5 / mov $F4,y / mov [$00]+y,a / inc y / bne data
 * FFF4 inc $01
 * FFF6 check: bpl data                        ; port 0 behind us: keep waiting
 * FFF8 cmp y,$F4 / bpl data                   ; port 0 jumped ahead: new block
 * FFFC bra block
 * FFFE reset vector
 */
static const u8 ipl_rom[64] = {
    0xCD,0xEF, 0xBD, 0xE8,0x00, 0xC6, 0x1D, 0xD0,0xFC,
    0xE8,0xAA, 0x8D,0xBB, 0xDA,0xF4, 0xE8,0xCC, 0x2E,0xF4,0xFD,
    0xBA,0xF6, 0xDA,0x00, 0xBA,0xF4, 0xC4,0xF4, 0xDD, 0x5D, 0xD0,0x03,
    0x1F,0x00,0x00,
    0xEB,0xF4, 0xD0,0xFC,
    0x7E,0xF4, 0xD0,0x0B,
    0xE4,0xF5, 0xCB,0xF4, 0xD7,0x00, 0xFC, 0xD0,0xF3,
    0xAB,0x01,
    0x10,0xEF, 0x7E,0xF4, 0x10,0xEB, 0x2F,0xD6,
    0xC0,0xFF
};

/* Base cycles per opcode; taken branches add 2. */
static const u8 cycle_tab[256] = {
/*        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0 */   2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,
/* 1 */   2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
/* 2 */   2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,
/* 3 */   2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
/* 4 */   2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,
/* 5 */   2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,
/* 6 */   2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,
/* 7 */   2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
/* 8 */   2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,
/* 9 */   2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2,12, 5,
/* A */   3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,
/* B */   2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
/* C */   3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,
/* D */   2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
/* E */   2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,
/* F */   2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,
};

/* ------------------------------------------------------------ memory, I/O */
static void dsp_write(u8 a, u8 v)
{
    dreg[a] = v;
    if (a == 0x4C) new_kon = v;
    else if (a == 0x7C) dreg[a] = 0;   /* any write to ENDX clears it */
}

static u8 io_read(int a)
{
    switch (a) {
    case 0xF2: return dsp_addr;
    case 0xF3: return dreg[dsp_addr & 0x7F];
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: return port_in[a - 0xF4];
    case 0xF8: case 0xF9: return aram[a];
    case 0xFD: case 0xFE: case 0xFF: {
        u8 v = tmr_out[a - 0xFD];
        tmr_out[a - 0xFD] = 0;
        return v;
    }
    default: return 0;                 /* $F0,$F1,$FA-$FC are write-only */
    }
}

static void io_write(int a, u8 v)
{
    switch (a) {
    case 0xF1:
        for (int i = 0; i < 3; i++)
            if ((v & (1 << i)) && !(ctrl & (1 << i))) {
                tmr_stage[i] = 0;
                tmr_out[i] = 0;
            }
        if (v & 0x10) port_in[0] = port_in[1] = 0;
        if (v & 0x20) port_in[2] = port_in[3] = 0;
        ipl_on = v >> 7;
        ctrl = v;
        break;
    case 0xF2: dsp_addr = v; break;
    case 0xF3: if (dsp_addr < 0x80) dsp_write(dsp_addr, v); break;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: port_out[a - 0xF4] = v; break;
    case 0xFA: case 0xFB: case 0xFC: tmr_target[a - 0xFA] = v; break;
    default: break;                    /* TEST, counters: nothing to do */
    }
}

static inline u8 rd(u16 a)
{
    if ((a & 0xFFF0) == 0x00F0) return io_read(a);
    if (a >= 0xFFC0 && ipl_on) return ipl_rom[a - 0xFFC0];
    return aram[a];
}

static inline void wr(u16 a, u8 v)
{
    aram[a] = v;                       /* RAM underneath always takes the write */
    if ((a & 0xFFF0) == 0x00F0) io_write(a, v);
}

/* Most MOV-to-memory forms read the target first; visible on $FD-$FF. */
static inline void wr_rmw(u16 a, u8 v) { (void)rd(a); wr(a, v); }

static inline u8  fetch(void)   { return rd(r_pc++); }
static inline u16 fetch16(void) { u16 l = fetch(); return (u16)(l | fetch() << 8); }
static inline u16 dp(int o)     { return (u16)((o & 0xFF) | (r_psw & F_P) << 3); }
static inline u16 ad_d(void)    { return dp(fetch()); }
static inline u16 ad_dx(void)   { return dp(fetch() + r_x); }
static inline u16 ad_dy(void)   { return dp(fetch() + r_y); }
static inline u16 ad_abs(void)  { return fetch16(); }
static inline u16 ad_absx(void) { return (u16)(fetch16() + r_x); }
static inline u16 ad_absy(void) { return (u16)(fetch16() + r_y); }
static inline u16 rd16dp(int o) { u16 l = rd(dp(o)); return (u16)(l | rd(dp(o + 1)) << 8); }
static inline u16 ad_dxi(void)  { return rd16dp(fetch() + r_x); }
static inline u16 ad_diy(void)  { return (u16)(rd16dp(fetch()) + r_y); }

static inline void push(u8 v) { wr((u16)(0x100 | r_sp), v); r_sp--; }
static inline u8   pop(void)  { r_sp++; return rd((u16)(0x100 | r_sp)); }
static inline void push16(u16 v) { push((u8)(v >> 8)); push((u8)v); }
static inline u16  pop16(void)   { u16 l = pop(); return (u16)(l | pop() << 8); }

static inline void set_flag(u8 f, int on) { r_psw = (u8)(on ? r_psw | f : r_psw & ~f); }
static inline void nz(u8 v)
{
    r_psw = (u8)((r_psw & ~(F_N | F_Z)) | (v & 0x80) | (v ? 0 : F_Z));
}
static inline void nz16(u16 v)
{
    r_psw = (u8)((r_psw & ~(F_N | F_Z)) | (v >> 8 & 0x80) | (v ? 0 : F_Z));
}

/* ------------------------------------------------------------------- ALU */
enum { OP_OR, OP_AND, OP_EOR, OP_CMP, OP_ADC, OP_SBC };

static inline u8 adc8(u8 a, u8 b)
{
    int c = r_psw & F_C;
    int r = a + b + c;
    set_flag(F_V, ~(a ^ b) & (a ^ r) & 0x80);
    set_flag(F_H, (a & 15) + (b & 15) + c > 15);
    set_flag(F_C, r > 0xFF);
    nz((u8)r);
    return (u8)r;
}

static inline u8 alu(int op, u8 a, u8 b)
{
    switch (op) {
    case OP_OR:  a |= b; nz(a); return a;
    case OP_AND: a &= b; nz(a); return a;
    case OP_EOR: a ^= b; nz(a); return a;
    case OP_CMP: set_flag(F_C, a >= b); nz((u8)(a - b)); return a;
    case OP_ADC: return adc8(a, b);
    default:     return adc8(a, (u8)~b);
    }
}

static inline void alu_mem(int op, u16 ad, u8 b)
{
    u8 r = alu(op, rd(ad), b);
    if (op != OP_CMP) wr(ad, r);
}

enum { SH_ASL, SH_ROL, SH_LSR, SH_ROR };

static inline u8 shift(int kind, u8 v)
{
    int c = r_psw & F_C;
    switch (kind) {
    case SH_ASL: set_flag(F_C, v & 0x80); v = (u8)(v << 1); break;
    case SH_ROL: set_flag(F_C, v & 0x80); v = (u8)(v << 1 | c); break;
    case SH_LSR: set_flag(F_C, v & 1);    v >>= 1; break;
    default:     set_flag(F_C, v & 1);    v = (u8)(v >> 1 | c << 7); break;
    }
    nz(v);
    return v;
}

static inline void shift_mem(int kind, u16 ad)  { wr(ad, shift(kind, rd(ad))); }
static inline void inc_mem(u16 ad) { u8 v = (u8)(rd(ad) + 1); nz(v); wr(ad, v); }
static inline void dec_mem(u16 ad) { u8 v = (u8)(rd(ad) - 1); nz(v); wr(ad, v); }

/* -------------------------------------------------------------- execution */
#define BRANCH(cond) do { s8 rel_ = (s8)fetch(); \
        if (cond) { r_pc = (u16)(r_pc + rel_); cyc += 2; } } while (0)

#define YA ((u16)(r_y << 8 | r_a))

/* The six ALU rows share one layout. */
#define ALU_ROW(base, op)                                                       \
    case (base) + 0x04: r_a = alu(op, r_a, rd(ad_d()));    break;               \
    case (base) + 0x05: r_a = alu(op, r_a, rd(ad_abs()));  break;               \
    case (base) + 0x06: r_a = alu(op, r_a, rd(dp(r_x)));   break;               \
    case (base) + 0x07: r_a = alu(op, r_a, rd(ad_dxi()));  break;               \
    case (base) + 0x08: r_a = alu(op, r_a, fetch());       break;               \
    case (base) + 0x09: { u8 s_ = rd(ad_d()); alu_mem(op, ad_d(), s_); break; } \
    case (base) + 0x14: r_a = alu(op, r_a, rd(ad_dx()));   break;               \
    case (base) + 0x15: r_a = alu(op, r_a, rd(ad_absx())); break;               \
    case (base) + 0x16: r_a = alu(op, r_a, rd(ad_absy())); break;               \
    case (base) + 0x17: r_a = alu(op, r_a, rd(ad_diy()));  break;               \
    case (base) + 0x18: { u8 s_ = fetch(); alu_mem(op, ad_d(), s_); break; }    \
    case (base) + 0x19: { u8 s_ = rd(dp(r_y)); alu_mem(op, dp(r_x), s_); break; }

static int smp_step(void)
{
    u8 op = fetch();
    int cyc = cycle_tab[op];
    u16 ad, w;
    u8 t;
    int bit;

    switch (op & 0x1F) {
    case 0x01: case 0x11:              /* TCALL n */
        ad = (u16)(0xFFDE - 2 * (op >> 4));
        push16(r_pc);
        { u16 l = rd(ad); r_pc = (u16)(l | rd((u16)(ad + 1)) << 8); }
        return cyc;
    case 0x02:                         /* SET1 d.b */
        ad = ad_d(); wr(ad, (u8)(rd(ad) | 1 << (op >> 5))); return cyc;
    case 0x12:                         /* CLR1 d.b */
        ad = ad_d(); wr(ad, (u8)(rd(ad) & ~(1 << (op >> 5)))); return cyc;
    case 0x03:                         /* BBS d.b,r */
        t = rd(ad_d()); BRANCH(t >> (op >> 5) & 1); return cyc;
    case 0x13:                         /* BBC d.b,r */
        t = rd(ad_d()); BRANCH(!(t >> (op >> 5) & 1)); return cyc;
    default: break;
    }

    switch (op) {
    ALU_ROW(0x00, OP_OR)
    ALU_ROW(0x20, OP_AND)
    ALU_ROW(0x40, OP_EOR)
    ALU_ROW(0x60, OP_CMP)
    ALU_ROW(0x80, OP_ADC)
    ALU_ROW(0xA0, OP_SBC)

    /* column 0: flags and branches */
    case 0x00: break;
    case 0x20: set_flag(F_P, 0); break;
    case 0x40: set_flag(F_P, 1); break;
    case 0x60: set_flag(F_C, 0); break;
    case 0x80: set_flag(F_C, 1); break;
    case 0xA0: set_flag(F_I, 1); break;
    case 0xC0: set_flag(F_I, 0); break;
    case 0xE0: r_psw &= (u8)~(F_V | F_H); break;
    case 0x10: BRANCH(!(r_psw & F_N)); break;
    case 0x30: BRANCH(r_psw & F_N); break;
    case 0x50: BRANCH(!(r_psw & F_V)); break;
    case 0x70: BRANCH(r_psw & F_V); break;
    case 0x90: BRANCH(!(r_psw & F_C)); break;
    case 0xB0: BRANCH(r_psw & F_C); break;
    case 0xD0: BRANCH(!(r_psw & F_Z)); break;
    case 0xF0: BRANCH(r_psw & F_Z); break;
    case 0x2F: BRANCH(1); cyc -= 2; break;   /* BRA: table already has the 4 */

    /* MOV A/X/Y <- memory */
    case 0xE4: r_a = rd(ad_d());    nz(r_a); break;
    case 0xE5: r_a = rd(ad_abs());  nz(r_a); break;
    case 0xE6: r_a = rd(dp(r_x));   nz(r_a); break;
    case 0xE7: r_a = rd(ad_dxi());  nz(r_a); break;
    case 0xE8: r_a = fetch();       nz(r_a); break;
    case 0xF4: r_a = rd(ad_dx());   nz(r_a); break;
    case 0xF5: r_a = rd(ad_absx()); nz(r_a); break;
    case 0xF6: r_a = rd(ad_absy()); nz(r_a); break;
    case 0xF7: r_a = rd(ad_diy());  nz(r_a); break;
    case 0xBF: r_a = rd(dp(r_x)); r_x++; nz(r_a); break;
    case 0xCD: r_x = fetch();       nz(r_x); break;
    case 0xF8: r_x = rd(ad_d());    nz(r_x); break;
    case 0xF9: r_x = rd(ad_dy());   nz(r_x); break;
    case 0xE9: r_x = rd(ad_abs());  nz(r_x); break;
    case 0x8D: r_y = fetch();       nz(r_y); break;
    case 0xEB: r_y = rd(ad_d());    nz(r_y); break;
    case 0xFB: r_y = rd(ad_dx());   nz(r_y); break;
    case 0xEC: r_y = rd(ad_abs());  nz(r_y); break;

    /* MOV memory <- A/X/Y */
    case 0xC4: wr_rmw(ad_d(), r_a);    break;
    case 0xC5: wr_rmw(ad_abs(), r_a);  break;
    case 0xC6: wr_rmw(dp(r_x), r_a);   break;
    case 0xC7: wr_rmw(ad_dxi(), r_a);  break;
    case 0xD4: wr_rmw(ad_dx(), r_a);   break;
    case 0xD5: wr_rmw(ad_absx(), r_a); break;
    case 0xD6: wr_rmw(ad_absy(), r_a); break;
    case 0xD7: wr_rmw(ad_diy(), r_a);  break;
    case 0xAF: wr(dp(r_x), r_a); r_x++; break;
    case 0xD8: wr_rmw(ad_d(), r_x);    break;
    case 0xD9: wr_rmw(ad_dy(), r_x);   break;
    case 0xC9: wr_rmw(ad_abs(), r_x);  break;
    case 0xCB: wr_rmw(ad_d(), r_y);    break;
    case 0xDB: wr_rmw(ad_dx(), r_y);   break;
    case 0xCC: wr_rmw(ad_abs(), r_y);  break;
    case 0x8F: t = fetch(); wr_rmw(ad_d(), t); break;
    case 0xFA: t = rd(ad_d()); wr(ad_d(), t);  break;

    /* register moves */
    case 0x5D: r_x = r_a; nz(r_x); break;
    case 0x7D: r_a = r_x; nz(r_a); break;
    case 0xDD: r_a = r_y; nz(r_a); break;
    case 0xFD: r_y = r_a; nz(r_y); break;
    case 0x9D: r_x = r_sp; nz(r_x); break;
    case 0xBD: r_sp = r_x; break;

    /* compare X/Y */
    case 0xC8: (void)alu(OP_CMP, r_x, fetch());     break;
    case 0x3E: (void)alu(OP_CMP, r_x, rd(ad_d()));  break;
    case 0x1E: (void)alu(OP_CMP, r_x, rd(ad_abs())); break;
    case 0xAD: (void)alu(OP_CMP, r_y, fetch());     break;
    case 0x7E: (void)alu(OP_CMP, r_y, rd(ad_d()));  break;
    case 0x5E: (void)alu(OP_CMP, r_y, rd(ad_abs())); break;

    /* shifts and rotates */
    case 0x0B: shift_mem(SH_ASL, ad_d());   break;
    case 0x1B: shift_mem(SH_ASL, ad_dx());  break;
    case 0x0C: shift_mem(SH_ASL, ad_abs()); break;
    case 0x1C: r_a = shift(SH_ASL, r_a);    break;
    case 0x2B: shift_mem(SH_ROL, ad_d());   break;
    case 0x3B: shift_mem(SH_ROL, ad_dx());  break;
    case 0x2C: shift_mem(SH_ROL, ad_abs()); break;
    case 0x3C: r_a = shift(SH_ROL, r_a);    break;
    case 0x4B: shift_mem(SH_LSR, ad_d());   break;
    case 0x5B: shift_mem(SH_LSR, ad_dx());  break;
    case 0x4C: shift_mem(SH_LSR, ad_abs()); break;
    case 0x5C: r_a = shift(SH_LSR, r_a);    break;
    case 0x6B: shift_mem(SH_ROR, ad_d());   break;
    case 0x7B: shift_mem(SH_ROR, ad_dx());  break;
    case 0x6C: shift_mem(SH_ROR, ad_abs()); break;
    case 0x7C: r_a = shift(SH_ROR, r_a);    break;

    /* increment and decrement */
    case 0x8B: dec_mem(ad_d());   break;
    case 0x9B: dec_mem(ad_dx());  break;
    case 0x8C: dec_mem(ad_abs()); break;
    case 0x9C: r_a--; nz(r_a); break;
    case 0x1D: r_x--; nz(r_x); break;
    case 0xDC: r_y--; nz(r_y); break;
    case 0xAB: inc_mem(ad_d());   break;
    case 0xBB: inc_mem(ad_dx());  break;
    case 0xAC: inc_mem(ad_abs()); break;
    case 0xBC: r_a++; nz(r_a); break;
    case 0x3D: r_x++; nz(r_x); break;
    case 0xFC: r_y++; nz(r_y); break;

    /* 16-bit */
    case 0xBA: w = rd16dp(fetch()); r_a = (u8)w; r_y = (u8)(w >> 8); nz16(w); break;
    case 0xDA: t = fetch();
        (void)rd(dp(t)); wr(dp(t), r_a); wr(dp(t + 1), r_y); break;
    case 0x3A: case 0x1A: t = fetch();
        w = (u16)(rd16dp(t) + (op == 0x3A ? 1 : -1));
        wr(dp(t), (u8)w); wr(dp(t + 1), (u8)(w >> 8)); nz16(w); break;
    case 0x7A: {
        u32 ya = YA, m = rd16dp(fetch()), r = ya + m;
        set_flag(F_V, ~(ya ^ m) & (ya ^ r) & 0x8000);
        set_flag(F_H, (ya & 0xFFF) + (m & 0xFFF) > 0xFFF);
        set_flag(F_C, r > 0xFFFF);
        r_a = (u8)r; r_y = (u8)(r >> 8); nz16((u16)r);
        break;
    }
    case 0x9A: {
        u32 ya = YA, m = rd16dp(fetch()), r = (ya - m) & 0xFFFF;
        set_flag(F_V, (ya ^ m) & (ya ^ r) & 0x8000);
        set_flag(F_H, (ya & 0xFFF) >= (m & 0xFFF));
        set_flag(F_C, ya >= m);
        r_a = (u8)r; r_y = (u8)(r >> 8); nz16((u16)r);
        break;
    }
    case 0x5A: {
        u16 ya = YA, m = rd16dp(fetch());
        set_flag(F_C, ya >= m);
        nz16((u16)(ya - m));
        break;
    }

    /* multiply, divide, decimal */
    case 0xCF: w = (u16)(r_y * r_a); r_a = (u8)w; r_y = (u8)(w >> 8); nz(r_y); break;
    case 0x9E: {
        u32 ya = YA, x = r_x, y = r_y;
        set_flag(F_H, (y & 15) >= (x & 15));
        set_flag(F_V, y >= x);
        if (y < x << 1) {
            r_a = (u8)(ya / x);
            r_y = (u8)(ya % x);
        } else {
            r_a = (u8)(255 - (ya - (x << 9)) / (256 - x));
            r_y = (u8)(x + (ya - (x << 9)) % (256 - x));
        }
        nz(r_a);
        break;
    }
    case 0xDF:
        if ((r_psw & F_C) || r_a > 0x99) { r_a += 0x60; set_flag(F_C, 1); }
        if ((r_psw & F_H) || (r_a & 15) > 9) r_a += 6;
        nz(r_a);
        break;
    case 0xBE:
        if (!(r_psw & F_C) || r_a > 0x99) { r_a -= 0x60; set_flag(F_C, 0); }
        if (!(r_psw & F_H) || (r_a & 15) > 9) r_a -= 6;
        nz(r_a);
        break;
    case 0x9F: r_a = (u8)(r_a >> 4 | r_a << 4); nz(r_a); break;

    /* mem.bit operations */
    case 0x0A: case 0x2A: case 0x4A: case 0x6A:
    case 0x8A: case 0xAA:
        w = fetch16();
        bit = rd(w & 0x1FFF) >> (w >> 13) & 1;
        switch (op) {
        case 0x0A: bit |= r_psw & F_C;  break;
        case 0x2A: bit = (bit ^ 1) | (r_psw & F_C); break;
        case 0x4A: bit &= r_psw & F_C;  break;
        case 0x6A: bit = (bit ^ 1) & (r_psw & F_C); break;
        case 0x8A: bit ^= r_psw & F_C;  break;
        default: break;
        }
        set_flag(F_C, bit);
        break;
    case 0xCA:
        w = fetch16(); ad = w & 0x1FFF;
        t = rd(ad);
        t = (u8)((t & ~(1 << (w >> 13))) | (r_psw & F_C) << (w >> 13));
        wr(ad, t);
        break;
    case 0xEA:
        w = fetch16(); ad = w & 0x1FFF;
        wr(ad, (u8)(rd(ad) ^ 1 << (w >> 13)));
        break;
    case 0x0E: case 0x4E:
        ad = ad_abs(); t = rd(ad);
        nz((u8)(r_a - t));
        wr(ad, (u8)(op == 0x0E ? t | r_a : t & ~r_a));
        break;

    /* carry */
    case 0xED: r_psw ^= F_C; break;

    /* compare-and-branch loops */
    case 0x2E: t = rd(ad_d());  BRANCH(r_a != t); break;
    case 0xDE: t = rd(ad_dx()); BRANCH(r_a != t); break;
    case 0x6E: ad = ad_d(); t = (u8)(rd(ad) - 1); wr(ad, t); BRANCH(t != 0); break;
    case 0xFE: r_y--; BRANCH(r_y != 0); break;

    /* stack */
    case 0x0D: push(r_psw); break;
    case 0x2D: push(r_a);   break;
    case 0x4D: push(r_x);   break;
    case 0x6D: push(r_y);   break;
    case 0x8E: r_psw = pop(); break;
    case 0xAE: r_a = pop();   break;
    case 0xCE: r_x = pop();   break;
    case 0xEE: r_y = pop();   break;

    /* flow */
    case 0x5F: r_pc = fetch16(); break;
    case 0x1F: { ad = ad_absx(); u16 l = rd(ad);
        r_pc = (u16)(l | rd((u16)(ad + 1)) << 8); break; }
    case 0x3F: w = fetch16(); push16(r_pc); r_pc = w; break;
    case 0x4F: t = fetch(); push16(r_pc); r_pc = (u16)(0xFF00 | t); break;
    case 0x6F: r_pc = pop16(); break;
    case 0x7F: r_psw = pop(); r_pc = pop16(); break;
    case 0x0F: {
        push16(r_pc);
        push(r_psw);
        set_flag(F_B, 1);
        set_flag(F_I, 0);
        u16 l = rd(0xFFDE);
        r_pc = (u16)(l | rd(0xFFDF) << 8);
        break;
    }
    case 0xEF: case 0xFF: smp_halted = 1; break;   /* SLEEP, STOP */
    default: break;
    }
    return cyc;
}

/* ------------------------------------------------------------ S-DSP tables */
static const s16 gauss_tab[512] = {
       0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
       1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,
       2,   2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,
       6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  10,
      11,  11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,
      18,  19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  27,  27,
      28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  36,  36,  37,  38,  39,  40,
      41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
      58,  59,  60,  61,  62,  64,  65,  66,  67,  69,  70,  71,  73,  74,  76,  77,
      78,  80,  81,  83,  84,  86,  87,  89,  90,  92,  94,  95,  97,  99, 100, 102,
     104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
     134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
     171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
     212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
     260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
     314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
     374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
     439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
     508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
     582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
     659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
     737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
     816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
     894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
     969, 974, 978, 983, 988, 992, 997,1001,1005,1010,1014,1019,1023,1027,1032,1036,
    1040,1045,1049,1053,1057,1061,1066,1070,1074,1078,1082,1086,1090,1094,1098,1102,
    1106,1109,1113,1117,1121,1125,1128,1132,1136,1139,1143,1146,1150,1153,1157,1160,
    1164,1167,1170,1174,1177,1180,1183,1186,1190,1193,1196,1199,1202,1205,1207,1210,
    1213,1216,1219,1221,1224,1227,1229,1232,1234,1237,1239,1241,1244,1246,1248,1251,
    1253,1255,1257,1259,1261,1263,1265,1267,1269,1270,1272,1274,1275,1277,1279,1280,
    1282,1283,1284,1286,1287,1288,1290,1291,1292,1293,1294,1295,1296,1297,1297,1298,
    1299,1300,1300,1301,1302,1302,1303,1303,1303,1304,1304,1304,1304,1304,1305,1305,
};

/* Envelope/noise rates: period in samples and phase within the shared counter. */
#define COUNTER_RANGE 30720
static const u16 rate_period[32] = {
    COUNTER_RANGE + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192,
    160, 128, 96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1
};
static const u16 rate_phase[32] = {
    1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0
};

static inline int rate_fires(int rate)
{
    return (env_counter + rate_phase[rate]) % rate_period[rate] == 0;
}

static inline int clamp16(int v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* ------------------------------------------------------------ S-DSP voices */
static void brr_decode(Voice *v, u8 header)
{
    int shift_n = header >> 4;
    int filter = header >> 2 & 3;
    u16 a = (u16)(v->brr_addr + v->brr_off);
    int nybbles = aram[a] << 8 | aram[(u16)(a + 1)];

    for (int i = 0; i < 4; i++, nybbles = nybbles << 4 & 0xFFFF) {
        int s = (s16)nybbles >> 12;
        s = (s * (1 << shift_n)) >> 1;
        if (shift_n >= 0xD) s = s < 0 ? -2048 : 0;   /* reserved shift values */

        int p1 = v->buf[(v->bufpos + 11) % 12];
        int p2 = v->buf[(v->bufpos + 10) % 12] >> 1;
        switch (filter) {
        case 1:
            s += p1 >> 1;
            s += (-p1) >> 5;
            break;
        case 2:
            s += p1 - p2;
            s += p2 >> 4;
            s += (p1 * -3) >> 6;
            break;
        case 3:
            s += p1 - p2;
            s += (p1 * -13) >> 7;
            s += (p2 * 3) >> 4;
            break;
        default:
            break;
        }
        s = clamp16(s);
        v->buf[v->bufpos] = (s16)(s * 2);    /* 15 bits: the doubling can wrap */
        v->bufpos = (v->bufpos + 1) % 12;
    }
}

static inline int gaussian(const Voice *v)
{
    int frac = v->interp >> 4 & 0xFF;
    const s16 *fwd = gauss_tab + 255 - frac;
    const s16 *rev = gauss_tab + frac;
    int p = v->bufpos + (v->interp >> 12);
    int out = (fwd[0] * v->buf[p % 12]) >> 11;
    out += (fwd[256] * v->buf[(p + 1) % 12]) >> 11;
    out += (rev[256] * v->buf[(p + 2) % 12]) >> 11;
    out = (s16)out;                          /* wraps at this point on the chip */
    out += (rev[0] * v->buf[(p + 3) % 12]) >> 11;
    return clamp16(out) & ~1;
}

static void run_envelope(Voice *v, const u8 *r)
{
    int env = v->env;

    if (v->env_mode == ENV_RELEASE) {
        env -= 8;
        v->env = env < 0 ? 0 : env;
        return;
    }

    int rate, sustain_src;
    u8 adsr0 = r[5];
    if (adsr0 & 0x80) {                      /* ADSR */
        sustain_src = r[6];
        if (v->env_mode == ENV_ATTACK) {
            rate = (adsr0 & 0x0F) * 2 + 1;
            env += rate < 31 ? 0x20 : 0x400;
        } else {
            env--;
            env -= env >> 8;
            rate = v->env_mode == ENV_DECAY ? (adsr0 >> 3 & 0x0E) + 0x10
                                            : r[6] & 0x1F;
        }
    } else {                                 /* GAIN */
        int gain = r[7], mode = gain >> 5;
        sustain_src = gain;
        if (mode < 4) {                      /* direct */
            env = gain * 0x10;
            rate = 31;
        } else {
            rate = gain & 0x1F;
            if (mode == 4) {                 /* linear decrease */
                env -= 0x20;
            } else if (mode == 5) {          /* exponential decrease */
                env--;
                env -= env >> 8;
            } else {                         /* linear, or bent line above 3/4 */
                env += (mode == 7 && (u32)v->hidden_env >= 0x600) ? 8 : 0x20;
            }
        }
    }

    if (v->env_mode == ENV_DECAY && env >> 8 == sustain_src >> 5)
        v->env_mode = ENV_SUSTAIN;
    v->hidden_env = env;

    if ((u32)env > 0x7FF) {                  /* unsigned: also catches underflow */
        env = env < 0 ? 0 : 0x7FF;
        if (v->env_mode == ENV_ATTACK) v->env_mode = ENV_DECAY;
    }
    if (rate_fires(rate)) v->env = env;
}

/* ------------------------------------------------------------- S-DSP frame */
static void ring_push(int l, int r)
{
    int wr_pos = (ring_rd + ring_count) % RING_FRAMES;
    ring[wr_pos * 2] = (s16)l;
    ring[wr_pos * 2 + 1] = (s16)r;
    if (ring_count < RING_FRAMES) ring_count++;
    else ring_rd = (ring_rd + 1) % RING_FRAMES;   /* full: drop the oldest */
}

static void dsp_sample(void)
{
    /* global counters */
    every_other ^= 1;
    if (every_other) {
        new_kon &= (u8)~kon_latch;
        kon_latch = new_kon;
        koff_latch = dreg[0x5C];
    }
    if (--env_counter < 0) env_counter = COUNTER_RANGE - 1;
    u8 flg = dreg[0x6C];
    if (rate_fires(flg & 0x1F)) {
        int fb = noise_lfsr << 13 ^ noise_lfsr << 14;
        noise_lfsr = (fb & 0x4000) ^ noise_lfsr >> 1;
    }

    u8 pmon = dreg[0x2D] & 0xFE, non = dreg[0x3D], eon = dreg[0x4D];
    int mix[2] = { 0, 0 }, wet[2] = { 0, 0 };
    int prev_out = 0;

    for (int i = 0; i < 8; i++) {
        Voice *v = &voice[i];
        u8 *r = &dreg[i << 4];
        u8 vbit = (u8)(1 << i);

        /* source directory: start pointer during KON, loop pointer after */
        u16 entry = (u16)((dreg[0x5D] << 8) + r[4] * 4 + (v->kon_delay ? 0 : 2));
        u16 next = (u16)(aram[entry] | aram[(u16)(entry + 1)] << 8);
        int pitch = r[2] | (r[3] & 0x3F) << 8;
        u8 header = aram[v->brr_addr];

        if (pmon & vbit) pitch += ((prev_out >> 5) * pitch) >> 10;

        if (v->kon_delay) {
            if (v->kon_delay == 5) {
                v->brr_addr = next;
                v->brr_off = 1;
                v->bufpos = 0;
                header = 0;                  /* ignored on this sample */
            }
            v->env = 0;
            v->hidden_env = 0;
            /* three forced BRR decodes fill the buffer before the sound starts */
            v->interp = (--v->kon_delay & 3) ? 0x4000 : 0;
            pitch = 0;
        }

        int s = gaussian(v);
        if (non & vbit) s = (s16)(noise_lfsr * 2);
        int out = ((s * v->env) >> 11) & ~1;
        u8 envx = (u8)(v->env >> 4);

        if ((flg & 0x80) || (header & 3) == 1) {     /* soft reset, or end */
            v->env_mode = ENV_RELEASE;
            v->env = 0;
        }
        if (every_other) {
            if (koff_latch & vbit) v->env_mode = ENV_RELEASE;
            if (kon_latch & vbit) {
                v->kon_delay = 5;
                v->env_mode = ENV_ATTACK;
                dreg[0x7C] &= (u8)~vbit;
            }
        }
        if (!v->kon_delay) run_envelope(v, r);

        if (v->interp >= 0x4000) {
            brr_decode(v, header);
            v->brr_off += 2;
            if (v->brr_off >= 9) {
                v->brr_addr = (u16)(v->brr_addr + 9);
                if (header & 1) {
                    v->brr_addr = next;
                    dreg[0x7C] |= vbit;
                }
                v->brr_off = 1;
            }
        }
        v->interp = (v->interp & 0x3FFF) + pitch;
        if (v->interp > 0x7FFF) v->interp = 0x7FFF;

        for (int ch = 0; ch < 2; ch++) {
            int amp = (out * (s8)r[ch]) >> 7;
            mix[ch] = clamp16(mix[ch] + amp);
            if (eon & vbit) wet[ch] = clamp16(wet[ch] + amp);
        }
        r[8] = envx;
        r[9] = (u8)(out >> 8);
        prev_out = out;
    }

    /* echo: read history, FIR, output, feedback, write back */
    u16 echo_ptr = (u16)((esa_latch << 8) + echo_offset);
    echo_hist_pos = (echo_hist_pos + 1) & 7;
    int final[2];
    for (int ch = 0; ch < 2; ch++) {
        u16 a = (u16)(echo_ptr + ch * 2);
        s16 e = (s16)(aram[a] | aram[(u16)(a + 1)] << 8);
        echo_hist[echo_hist_pos][ch] = e >> 1;

        int acc = 0;
        for (int t = 0; t < 7; t++)
            acc += (echo_hist[(echo_hist_pos + t + 1) & 7][ch] * (s8)dreg[t * 0x10 + 0x0F]) >> 6;
        acc = (s16)acc;                      /* first seven taps wrap */
        acc += (s16)((echo_hist[echo_hist_pos][ch] * (s8)dreg[0x7F]) >> 6);
        int echo_in = clamp16(acc) & ~1;

        final[ch] = clamp16((s16)((mix[ch] * (s8)dreg[ch * 0x10 + 0x0C]) >> 7)
                          + (s16)((echo_in * (s8)dreg[ch * 0x10 + 0x2C]) >> 7));
        int fb = clamp16(wet[ch] + (s16)((echo_in * (s8)dreg[0x0D]) >> 7)) & ~1;
        if (!(flg & 0x20)) {
            aram[a] = (u8)fb;
            aram[(u16)(a + 1)] = (u8)(fb >> 8);
        }
    }
    esa_latch = dreg[0x6D];
    if (!echo_offset) echo_length = (dreg[0x7D] & 0x0F) * 0x800;
    echo_offset += 4;
    if (echo_offset >= echo_length) echo_offset = 0;

    if (flg & 0x40) ring_push(0, 0);         /* mute */
    else ring_push(final[0], final[1]);
}

/* ----------------------------------------------------------------- clocks */
static inline void clock_timer(int i)
{
    if (!(ctrl & (1 << i))) return;
    tmr_stage[i]++;                          /* u8: target 0 acts as 256 */
    if (tmr_stage[i] == tmr_target[i]) {
        tmr_stage[i] = 0;
        tmr_out[i] = (tmr_out[i] + 1) & 15;
    }
}

static inline void clock_units(int cycles)
{
    tmr_div_slow += cycles;
    while (tmr_div_slow >= 128) {            /* 8 kHz */
        tmr_div_slow -= 128;
        clock_timer(0);
        clock_timer(1);
    }
    tmr_div_fast += cycles;
    while (tmr_div_fast >= 16) {             /* 64 kHz */
        tmr_div_fast -= 16;
        clock_timer(2);
    }
    dsp_div += cycles;
    while (dsp_div >= 32) {
        dsp_div -= 32;
        dsp_sample();
    }
}

/* -------------------------------------------------------------- public API */
void snes_apu_reset(void)
{
    /* master_last is deliberately kept: see the resync in snes_apu_run_to. */
    memset(aram, 0, sizeof aram);
    r_a = r_x = r_y = r_sp = 0;
    r_psw = F_Z;
    smp_halted = 0;
    ipl_on = 1;
    ctrl = 0x80;
    dsp_addr = 0;
    memset(port_in, 0, sizeof port_in);
    memset(port_out, 0, sizeof port_out);
    memset(tmr_target, 0, sizeof tmr_target);
    memset(tmr_stage, 0, sizeof tmr_stage);
    memset(tmr_out, 0, sizeof tmr_out);
    tmr_div_slow = tmr_div_fast = dsp_div = 0;
    master_frac = cycle_budget = 0;

    memset(dreg, 0, sizeof dreg);
    dreg[0x6C] = 0xE0;                       /* soft reset, mute, echo off */
    memset(voice, 0, sizeof voice);
    new_kon = kon_latch = koff_latch = every_other = 0;
    env_counter = 0;
    noise_lfsr = 0x4000;
    echo_offset = echo_length = echo_hist_pos = 0;
    memset(echo_hist, 0, sizeof echo_hist);
    esa_latch = 0;

    ring_rd = ring_count = 0;

    r_pc = (u16)(rd(0xFFFE) | rd(0xFFFF) << 8);
}

u8 snes_apu_read(int port)
{
    return port_out[port & 3];
}

void snes_apu_write(int port, u8 v)
{
    port_in[port & 3] = v;
}

void snes_apu_rebase(s32 by)
{
    master_last -= by;
}

void snes_apu_run_to(s32 master)
{
    s32 delta = (s32)((u32)master - (u32)master_last);
    /* A count that restarted (new game, master cleared) simply resyncs. */
    master_last = master;
    if (delta <= 0) return;

    if (delta < 2000) master_frac += (u32)delta * 1024000u;   /* stays in 32 bits, which divide far faster */
    else {
        u64 f = (u64)master_frac + (u64)delta * 1024000u;
        cycle_budget += (s32)(f / 21477272u);
        f %= 21477272u;
        master_frac = (u32)f;
    }
    u32 cycles = master_frac / 21477272u;
    master_frac -= cycles * 21477272u;
    cycle_budget += (s32)cycles;

    while (cycle_budget > 0) {
        int c = smp_halted ? 2 : smp_step();
        clock_units(c);
        cycle_budget -= c;
    }
}

int snes_apu_take(s16 *out, int max)
{
    int n = max < ring_count ? max : ring_count;
    for (int i = 0; i < n; i++) {
        out[i * 2] = ring[ring_rd * 2];
        out[i * 2 + 1] = ring[ring_rd * 2 + 1];
        ring_rd = (ring_rd + 1) % RING_FRAMES;
    }
    ring_count -= n;
    return n;
}

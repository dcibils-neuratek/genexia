/*
 * ym2612.c - Yamaha YM2612 (OPN2) FM synthesizer.
 *
 * Runs at the chip's native rate (68k clock / 144 = 53267 Hz), one call of
 * ym_tick() per output sample. Uses the real chip's number domains:
 *   - 20-bit phase accumulators, 10-bit sine index
 *   - quarter-wave log-sin ROM (256 x 12 bit) + exp ROM (256 entries)
 *   - 10-bit envelope attenuation (0.09375 dB/step), TL << 3, SL << 5
 *   - envelope generator clocked every 3 samples with the hardware
 *     rate/shift/increment pattern tables
 *   - 14-bit operator outputs, 9-bit channel DAC output
 */
#include "genesis.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { EG_OFF = 0, EG_REL, EG_SUS, EG_DEC, EG_ATT };

typedef struct {
    u8 dt, mul, tl, ks, ar, d1r, d2r, rr, am, ssg;
    u16 sl;            /* sustain level in envelope units */
    u32 phase;         /* 20-bit */
    int env;           /* 0..1023 attenuation */
    int state;
    u8 key;            /* bit0: reg 0x28, bit1: CSM */
    u8 ssgn;           /* SSG-EG inversion toggle */
} Op;

typedef struct {
    Op op[4];          /* logical order op1..op4 */
    u16 fnum;
    u8 blk;
    u8 alg, fb;
    u8 left, right;
    u8 ams, pms;
    int fb1, fb2;      /* op1 output history for feedback */
} Chan;

static struct {
    Chan ch[6];
    u8 addr[2];
    u8 fn_latch, sl3_latch;
    u16 sl3_fnum[3];
    u8 sl3_blk[3];
    u8 lfo_en, lfo_freq;
    int lfo_cnt, lfo_sub;
    u8 mode27;
    u16 ta;
    u8 tb;
    int ta_cnt, tb_cnt, tb_sub;
    u8 status;
    u8 dac_en, dac;
    u32 eg_cnt;
    int eg_sub;
    u8 csm_pending;
} ym EMU_STATE;

/* ------------------------------------------------------------ tables */
static u16 sintab[256];   /* -log2(sin) * 256, quarter wave */
static u16 exptab[256];   /* 2^(-i/256) scaled to 13 bits */
static int tables_ready;

static const u8 dt_tab[4][32] = {
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8 },
    { 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16 },
    { 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22 },
};

static const u8 eg_inc_lo[4][8] = {
    { 0,1,0,1,0,1,0,1 },
    { 0,1,0,1,1,1,0,1 },
    { 0,1,1,1,0,1,1,1 },
    { 0,1,1,1,1,1,1,1 },
};
static const u8 eg_inc_hi[16][8] = {
    { 1,1,1,1,1,1,1,1 }, { 1,1,1,2,1,1,1,2 }, { 1,2,1,2,1,2,1,2 }, { 1,2,2,2,1,2,2,2 },
    { 2,2,2,2,2,2,2,2 }, { 2,2,2,4,2,2,2,4 }, { 2,4,2,4,2,4,2,4 }, { 2,4,4,4,2,4,4,4 },
    { 4,4,4,4,4,4,4,4 }, { 4,4,4,8,4,4,4,8 }, { 4,8,4,8,4,8,4,8 }, { 4,8,8,8,4,8,8,8 },
    { 8,8,8,8,8,8,8,8 }, { 8,8,8,8,8,8,8,8 }, { 8,8,8,8,8,8,8,8 }, { 8,8,8,8,8,8,8,8 },
};

/* LFO: samples per step (128 steps per period) */
static const u8 lfo_period[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };
static const u8 ams_shift[4] = { 8, 3, 1, 0 };
static const u8 pm_sh1[8][8] = {
    {7,7,7,7,7,7,7,7}, {7,7,7,7,7,7,7,7}, {7,7,7,7,7,7,1,1}, {7,7,7,7,1,1,1,1},
    {7,7,7,1,1,1,1,0}, {7,7,1,1,0,0,0,0}, {7,7,1,1,0,0,0,0}, {7,7,1,1,0,0,0,0},
};
static const u8 pm_sh2[8][8] = {
    {7,7,7,7,7,7,7,7}, {7,7,7,7,2,2,2,2}, {7,7,7,2,2,2,7,7}, {7,7,2,2,7,7,2,2},
    {7,7,2,7,7,7,2,7}, {7,7,7,2,7,7,2,1}, {7,7,7,2,7,7,2,1}, {7,7,7,2,7,7,2,1},
};

static void init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        double s = sin(((double)i + 0.5) * M_PI / 512.0);
        int v = (int)lround(-log2(s) * 256.0);
        sintab[i] = (u16)(v > 4095 ? 4095 : v);
        exptab[i] = (u16)lround(8192.0 * pow(2.0, -((double)i + 1.0) / 256.0));
    }
    tables_ready = 1;
}

/* ------------------------------------------------------------ helpers */
static inline int keycode(u16 fnum, u8 blk)
{
    int f11 = (fnum >> 10) & 1;
    int lo = (fnum >> 7) & 7;
    int n3 = f11 ? (lo != 0) : (lo == 7);
    return (blk << 2) | (f11 << 1) | n3;
}

/* fnum/block used by logical operator o of channel c (channel 3 special mode) */
static inline void op_freq(int c, int o, u16 *fnum, u8 *blk)
{
    if (c == 2 && (ym.mode27 & 0xC0) && o != 3) {
        static const int map[3] = { 1, 2, 0 };   /* op1<-A9, op2<-AA, op3<-A8 */
        *fnum = ym.sl3_fnum[map[o]];
        *blk = ym.sl3_blk[map[o]];
    } else {
        *fnum = ym.ch[c].fnum;
        *blk = ym.ch[c].blk;
    }
}

static inline int calc_rate(int r, int ksr)
{
    if (!r) return 0;
    int rate = 2 * r + ksr;
    return rate > 63 ? 63 : rate;
}

static inline int eg_inc(int rate)
{
    if (rate < 2) return 0;
    if (rate < 48) {
        int shift = 11 - (rate >> 2);
        if (ym.eg_cnt & ((1u << shift) - 1)) return 0;
        return eg_inc_lo[rate & 3][(ym.eg_cnt >> shift) & 7];
    }
    return eg_inc_hi[rate - 48][ym.eg_cnt & 7];
}

static void op_key_on(Op *o, int kc)
{
    o->phase = 0;
    o->ssgn = 0;
    int rate = calc_rate(o->ar, kc >> (3 - o->ks));
    if (rate >= 62) {
        o->env = 0;
        o->state = o->sl ? EG_DEC : EG_SUS;
    } else {
        o->state = EG_ATT;
    }
}

static void op_key_off(Op *o)
{
    if (o->state <= EG_REL) return;
    if (o->ssg & 8) {
        if (o->ssgn ^ ((o->ssg >> 2) & 1))
            o->env = (0x200 - o->env) & 0x3ff;
        if (o->env >= 0x200) { o->env = 1023; o->state = EG_OFF; return; }
    }
    o->state = EG_REL;
}

static void set_key(int c, int o, int bit, int on)
{
    Op *op = &ym.ch[c].op[o];
    u8 old = op->key;
    op->key = on ? (u8)(old | bit) : (u8)(old & ~bit);
    if (!old && op->key) {
        u16 f; u8 b;
        op_freq(c, o, &f, &b);
        op_key_on(op, keycode(f, b));
    } else if (old && !op->key) {
        op_key_off(op);
    }
}

static void eg_update(Op *o, int kc)
{
    int ksr = kc >> (3 - o->ks);
    int inc;
    switch (o->state) {
    case EG_ATT: {
        int rate = calc_rate(o->ar, ksr);
        if (rate >= 62) { o->env = 0; }
        else if ((inc = eg_inc(rate)) != 0) o->env += (~o->env * inc) >> 4;
        if (o->env <= 0) { o->env = 0; o->state = o->sl ? EG_DEC : EG_SUS; }
        break;
    }
    case EG_DEC:
        inc = eg_inc(calc_rate(o->d1r, ksr));
        if (o->ssg & 8) { if (o->env < 0x200) o->env += inc * 4; }
        else o->env += inc;
        if (o->env >= o->sl) o->state = EG_SUS;
        if (o->env > 1023) o->env = 1023;
        break;
    case EG_SUS:
        inc = eg_inc(calc_rate(o->d2r, ksr));
        if (o->ssg & 8) { if (o->env < 0x200) o->env += inc * 4; }
        else o->env += inc;
        if (o->env > 1023) o->env = 1023;
        break;
    case EG_REL:
        inc = eg_inc(calc_rate(o->rr * 2 + 1, ksr));
        if ((o->ssg & 8) && o->env < 0x200) inc *= 4;
        o->env += inc;
        if (o->env >= 1023) { o->env = 1023; o->state = EG_OFF; }
        break;
    default:
        break;
    }
}

static void ssg_update(Op *o)
{
    if (!(o->ssg & 8) || o->env < 0x200 || o->state <= EG_REL) return;
    if (o->ssg & 1) {                  /* hold */
        if (o->ssg & 2) o->ssgn = 1;
        if (o->state != EG_ATT && !(o->ssgn ^ ((o->ssg >> 2) & 1))) o->env = 1023;
    } else {
        if (o->ssg & 2) o->ssgn ^= 1;
        else o->phase = 0;
        if (o->state != EG_ATT) {
            if (o->ar >= 31) { o->env = 0; o->state = o->sl ? EG_DEC : EG_SUS; }
            else o->state = EG_ATT;
        }
    }
}

static inline int op_vol(const Op *o, int am)
{
    int v = o->env;
    if ((o->ssg & 8) && o->state > EG_REL && (o->ssgn ^ ((o->ssg >> 2) & 1)))
        v = (0x200 - v) & 0x3ff;
    v += o->tl << 3;
    if (o->am) v += am;
    return v > 1023 ? 1023 : v;
}

/* sine lookup + exp: returns 14-bit signed output */
static inline int op_calc(u32 phase, int mod, int vol)
{
    if (vol >= 1023) return 0;
    int i = (int)((phase >> 10) + (u32)mod) & 1023;
    int q = i & 255;
    if (i & 256) q ^= 255;
    int att = sintab[q] + (vol << 2);
    if (att >= 13 * 256) return 0;
    int v = exptab[att & 255] >> (att >> 8);
    return (i & 512) ? -v : v;
}

static inline u32 phase_inc(const Op *o, u16 fnum, u8 blk, int pms, int lfo_pm)
{
    u32 f = (u32)fnum << 1;
    if (pms) {
        u32 fh = fnum >> 4;
        int l = lfo_pm & 0x0f;
        if (l & 8) l ^= 0x0f;
        u32 fm = (fh >> pm_sh1[pms][l]) + (fh >> pm_sh2[pms][l]);
        if (pms > 5) fm <<= pms - 5;
        fm >>= 2;
        if (lfo_pm & 0x10) f -= fm; else f += fm;
        f &= 0xfff;
    }
    u32 base = (f << blk) >> 2;
    int d = dt_tab[o->dt & 3][keycode(fnum, blk)];
    if (o->dt & 4) base -= (u32)d; else base += (u32)d;
    base &= 0x1ffff;
    u32 inc = o->mul ? base * o->mul : base >> 1;
    return inc & 0xfffff;
}

/* ------------------------------------------------------------ channel */
static int chan_calc(int c, int am_lfo, int pm_lfo)
{
    Chan *ch = &ym.ch[c];
    int am = am_lfo >> ams_shift[ch->ams];
    int pms = ym.lfo_en ? ch->pms : 0;
    int vol[4];
    u32 ph[4];
    for (int o = 0; o < 4; o++) {
        Op *op = &ch->op[o];
        u16 f; u8 b;
        op_freq(c, o, &f, &b);
        vol[o] = op_vol(op, am);
        ph[o] = op->phase;
        op->phase = (op->phase + phase_inc(op, f, b, pms, pm_lfo)) & 0xfffff;
    }

    int fbmod = ch->fb ? (ch->fb1 + ch->fb2) >> (10 - ch->fb) : 0;
    int o1 = op_calc(ph[0], fbmod, vol[0]);
    ch->fb2 = ch->fb1;
    ch->fb1 = o1;
    int o2, o3, o4, out;
    switch (ch->alg) {
    case 0:
        o2 = op_calc(ph[1], o1 >> 1, vol[1]);
        o3 = op_calc(ph[2], o2 >> 1, vol[2]);
        out = op_calc(ph[3], o3 >> 1, vol[3]);
        break;
    case 1:
        o2 = op_calc(ph[1], 0, vol[1]);
        o3 = op_calc(ph[2], (o1 + o2) >> 1, vol[2]);
        out = op_calc(ph[3], o3 >> 1, vol[3]);
        break;
    case 2:
        o2 = op_calc(ph[1], 0, vol[1]);
        o3 = op_calc(ph[2], o2 >> 1, vol[2]);
        out = op_calc(ph[3], (o1 + o3) >> 1, vol[3]);
        break;
    case 3:
        o2 = op_calc(ph[1], o1 >> 1, vol[1]);
        o3 = op_calc(ph[2], 0, vol[2]);
        out = op_calc(ph[3], (o2 + o3) >> 1, vol[3]);
        break;
    case 4:
        o2 = op_calc(ph[1], o1 >> 1, vol[1]);
        o3 = op_calc(ph[2], 0, vol[2]);
        o4 = op_calc(ph[3], o3 >> 1, vol[3]);
        out = o2 + o4;
        break;
    case 5:
        o2 = op_calc(ph[1], o1 >> 1, vol[1]);
        o3 = op_calc(ph[2], o1 >> 1, vol[2]);
        o4 = op_calc(ph[3], o1 >> 1, vol[3]);
        out = o2 + o3 + o4;
        break;
    case 6:
        o2 = op_calc(ph[1], o1 >> 1, vol[1]);
        o3 = op_calc(ph[2], 0, vol[2]);
        o4 = op_calc(ph[3], 0, vol[3]);
        out = o2 + o3 + o4;
        break;
    default:
        o2 = op_calc(ph[1], 0, vol[1]);
        o3 = op_calc(ph[2], 0, vol[2]);
        o4 = op_calc(ph[3], 0, vol[3]);
        out = o1 + o2 + o3 + o4;
        break;
    }
    if (out > 8191) out = 8191;
    if (out < -8192) out = -8192;
    return out;
}

/* ------------------------------------------------------------ registers */
static void write_op(int c, int o, int reg, u8 v)
{
    Op *op = &ym.ch[c].op[o];
    switch (reg & 0xF0) {
    case 0x30: op->dt = (v >> 4) & 7; op->mul = v & 15; break;
    case 0x40: op->tl = v & 0x7f; break;
    case 0x50: op->ks = v >> 6; op->ar = v & 31; break;
    case 0x60: op->am = v >> 7; op->d1r = v & 31; break;
    case 0x70: op->d2r = v & 31; break;
    case 0x80: {
        int s = v >> 4;
        op->sl = (u16)(s == 15 ? 0x3E0 : s << 5);
        op->rr = v & 15;
        if (op->state == EG_DEC && op->env >= op->sl) op->state = EG_SUS;
        break;
    }
    case 0x90: op->ssg = v & 15; break;
    default: break;
    }
}

static void write_reg(int part, u8 r, u8 v)
{
    if (part == 0 && r < 0x30) {
        switch (r) {
        case 0x22:
            ym.lfo_en = (v >> 3) & 1;
            ym.lfo_freq = v & 7;
            if (!ym.lfo_en) { ym.lfo_cnt = 0; ym.lfo_sub = 0; }
            break;
        case 0x24: ym.ta = (u16)((ym.ta & 3) | (v << 2)); break;
        case 0x25: ym.ta = (u16)((ym.ta & 0x3fc) | (v & 3)); break;
        case 0x26: ym.tb = v; break;
        case 0x27: {
            u8 old = ym.mode27;
            if ((v & 1) && !(old & 1)) ym.ta_cnt = ym.ta;
            if ((v & 2) && !(old & 2)) { ym.tb_cnt = ym.tb; ym.tb_sub = 0; }
            if (v & 0x10) ym.status &= (u8)~1;
            if (v & 0x20) ym.status &= (u8)~2;
            ym.mode27 = v & 0xCF;
            break;
        }
        case 0x28: {
            int c = v & 3;
            if (c == 3) break;
            if (v & 4) c += 3;
            for (int o = 0; o < 4; o++) set_key(c, o, 1, (v >> (4 + o)) & 1);
            break;
        }
        case 0x2A: ym.dac = v; break;
        case 0x2B: ym.dac_en = v >> 7; break;
        default: break;
        }
        return;
    }
    if (r < 0x30) return;
    int cr = r & 3;
    if (cr == 3) return;
    int c = cr + part * 3;
    Chan *ch = &ym.ch[c];
    if (r < 0xA0) {
        static const int slot_map[4] = { 0, 2, 1, 3 };   /* reg order op1,op3,op2,op4 */
        write_op(c, slot_map[(r >> 2) & 3], r, v);
        return;
    }
    switch (r & 0xFC) {
    case 0xA0: ch->fnum = (u16)(((ym.fn_latch & 7) << 8) | v); ch->blk = (ym.fn_latch >> 3) & 7; break;
    case 0xA4: ym.fn_latch = v & 0x3f; break;
    case 0xA8:
        if (part == 0) {
            ym.sl3_fnum[cr] = (u16)(((ym.sl3_latch & 7) << 8) | v);
            ym.sl3_blk[cr] = (ym.sl3_latch >> 3) & 7;
        }
        break;
    case 0xAC: if (part == 0) ym.sl3_latch = v & 0x3f; break;
    case 0xB0: ch->fb = (v >> 3) & 7; ch->alg = v & 7; break;
    case 0xB4:
        ch->left = v >> 7; ch->right = (v >> 6) & 1;
        ch->ams = (v >> 4) & 3; ch->pms = v & 7;
        break;
    default: break;
    }
}

/* ------------------------------------------------------------ API */
void ym2612_reset(void)
{
    if (!tables_ready) init_tables();
    memset(&ym, 0, sizeof ym);
    for (int c = 0; c < 6; c++) {
        ym.ch[c].left = ym.ch[c].right = 1;
        for (int o = 0; o < 4; o++) {
            ym.ch[c].op[o].env = 1023;
            ym.ch[c].op[o].state = EG_OFF;
        }
    }
    ym.eg_cnt = 1;
}

void ym2612_write(int port, u8 v)
{
    if (!tables_ready) ym2612_reset();
    switch (port & 3) {
    case 0: ym.addr[0] = v; break;
    case 1: write_reg(0, ym.addr[0], v); break;
    case 2: ym.addr[1] = v; break;
    default: write_reg(1, ym.addr[1], v); break;
    }
}

u8 ym2612_read_status(void) { return ym.status & 3; }

static void timers_tick(void)
{
    if (ym.csm_pending) {          /* CSM key-on lasts one sample */
        ym.csm_pending = 0;
        for (int o = 0; o < 4; o++) set_key(2, o, 2, 0);
    }
    if (ym.mode27 & 1) {
        if (++ym.ta_cnt >= 1024) {
            ym.ta_cnt = ym.ta;
            if (ym.mode27 & 4) ym.status |= 1;
            if ((ym.mode27 & 0xC0) == 0x80) {
                for (int o = 0; o < 4; o++) set_key(2, o, 2, 1);
                ym.csm_pending = 1;
            }
        }
    }
    if (ym.mode27 & 2) {
        if (++ym.tb_sub >= 16) {
            ym.tb_sub = 0;
            if (++ym.tb_cnt >= 256) {
                ym.tb_cnt = ym.tb;
                if (ym.mode27 & 8) ym.status |= 2;
            }
        }
    }
}

/* advance only the timers (status flags the game can read) by n samples;
 * used for frames whose audio is thrown away (run-ahead) */
void ym2612_advance(int n)
{
    if (!tables_ready) ym2612_reset();
    for (int i = 0; i < n; i++) timers_tick();
}

void ym2612_render(s16 *out, int n)
{
    if (!tables_ready) ym2612_reset();
    for (int i = 0; i < n; i++) {
        timers_tick();

        /* LFO */
        int am_lfo = 0, pm_lfo = 0;
        if (ym.lfo_en) {
            if (++ym.lfo_sub >= lfo_period[ym.lfo_freq]) {
                ym.lfo_sub = 0;
                ym.lfo_cnt = (ym.lfo_cnt + 1) & 127;
            }
            am_lfo = (ym.lfo_cnt < 64 ? (ym.lfo_cnt & 63) : (63 - (ym.lfo_cnt & 63))) * 2;
            pm_lfo = ym.lfo_cnt >> 2;
        }

        /* envelope generator: every 3 samples */
        if (++ym.eg_sub >= 3) {
            ym.eg_sub = 0;
            if (++ym.eg_cnt >= 4096) ym.eg_cnt = 1;
            for (int c = 0; c < 6; c++)
                for (int o = 0; o < 4; o++) {
                    if (ym.ch[c].op[o].state == EG_OFF) continue;   /* nothing to advance */
                    u16 f; u8 b;
                    op_freq(c, o, &f, &b);
                    eg_update(&ym.ch[c].op[o], keycode(f, b));
                }
        }
        for (int c = 0; c < 6; c++)
            for (int o = 0; o < 4; o++) ssg_update(&ym.ch[c].op[o]);

        int l = 0, r = 0;
        for (int c = 0; c < 6; c++) {
            Chan *chp = &ym.ch[c];
            int v;
            /* fully released channel: output is silence and key-on resets the
             * phase, so skipping the synthesis changes nothing audible */
            if (chp->op[0].state == EG_OFF && chp->op[1].state == EG_OFF &&
                chp->op[2].state == EG_OFF && chp->op[3].state == EG_OFF) {
                v = 0; chp->fb1 = chp->fb2 = 0;
            } else v = chan_calc(c, am_lfo, pm_lfo);
            if (c == 5 && ym.dac_en) v = ((int)ym.dac - 128) << 6;
            v >>= 5;                      /* 9-bit DAC */
            if (ym.ch[c].left) l += v;
            if (ym.ch[c].right) r += v;
        }
        /* 6 x 9-bit channels -> max +-1536; scale for headroom */
        l *= 11; r *= 11;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        out[2 * i] = (s16)l;
        out[2 * i + 1] = (s16)r;
    }
}

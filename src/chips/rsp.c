/*
 * rsp.c - Nintendo 64 Reality Signal Processor, low-level interpreter.
 *
 * Runs real microcode from IMEM: a MIPS-like scalar unit plus the COP2 vector
 * unit. One instruction costs one cycle.
 *
 * Vector registers are arrays of 8 host-order u16, index 0 being VU element 0
 * (the most significant one in DMEM order). The accumulator is split into
 * three u16 slices, and each flag register into 8 lanes holding 0 or 1: the
 * per-element loops below are plain C that clang turns into Neon code, working
 * on 32-bit "high:mid" values, so no 64-bit lanes are needed. In flag
 * registers as read by CFC2, bit n is element n.
 */
#include "n64.h"
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

typedef struct {
    u16 vr[32][8];
    u16 acch[8], accm[8], accl[8];
    u16 vcol[8], vcoh[8];          /* VCO: carry (bits 0-7), not-equal (8-15) */
    u16 vccl[8], vcch[8];          /* VCC: compare (0-7), clip (8-15) */
    u16 vce[8];
    u32 gpr[32];
    u32 pc;                        /* next instruction, offset in IMEM */
    u32 target;                    /* where to go after the delay slot */
    u16 div_in, div_out;
    u8  div_dp;                    /* VRCPH/VRSQH primed a double precision op */
    u8  delay;                     /* next instruction is in a delay slot */
} RspState;

static RspState R N64_STATE __attribute__((aligned(16)));

/* Divide ROMs. Derived data, the same for every run, hence not N64_STATE. */
static u16 rcp_rom[512], rsq_rom[512];

__attribute__((constructor)) static void rsp_build_tables(void)
{
    rcp_rom[0] = 0xFFFF;
    for (u32 i = 1; i < 512; i++) {
        u64 b = (1ull << 34) / (i + 512);
        rcp_rom[i] = (u16)((b + 1) >> 8);
    }
    for (u32 i = 0; i < 512; i++) {
        u64 a = (i + 512) >> (i & 1);
        /* Smallest b from 2^17 with a * (b + 1)^2 >= 2^44, by bisection: this
         * runs at startup, whichever console is being played. */
        u64 lo = 1 << 17, hi = 1 << 19;
        while (lo < hi) {
            u64 mid = (lo + hi) / 2;
            if (a * (mid + 1) * (mid + 1) < (1ull << 44)) lo = mid + 1;
            else hi = mid;
        }
        rsq_rom[i] = (u16)(lo >> 1);
    }
}

/* ------------------------------------------------------------------ DMEM */
#define dmem n64_sp_mem

static inline u8 dm_r8(u32 a) { return dmem[a & 0xFFF]; }
static inline void dm_w8(u32 a, u8 v) { dmem[a & 0xFFF] = v; }
static inline u16 dm_r16(u32 a) { return (u16)(dm_r8(a) << 8 | dm_r8(a + 1)); }
static inline void dm_w16(u32 a, u32 v) { dm_w8(a, (u8)(v >> 8)); dm_w8(a + 1, (u8)v); }

static inline u32 dm_r32(u32 a)
{
    a &= 0xFFF;
    if (a <= 0xFFC) {
        u32 v;
        memcpy(&v, dmem + a, 4);
        return __builtin_bswap32(v);
    }
    return (u32)dm_r8(a) << 24 | (u32)dm_r8(a + 1) << 16 | (u32)dm_r8(a + 2) << 8 | dm_r8(a + 3);
}

static inline void dm_w32(u32 a, u32 v)
{
    a &= 0xFFF;
    if (a <= 0xFFC) {
        v = __builtin_bswap32(v);
        memcpy(dmem + a, &v, 4);
        return;
    }
    dm_w8(a, (u8)(v >> 24)); dm_w8(a + 1, (u8)(v >> 16));
    dm_w8(a + 2, (u8)(v >> 8)); dm_w8(a + 3, (u8)v);
}

/* ---------------------------------------------------- vector register bytes
 * Byte n of a register, in DMEM order: byte 0 is the high half of element 0. */
static inline u8 vb_get(const u16 *v, int n)
{
    n &= 15;
    return (u8)((n & 1) ? v[n >> 1] : v[n >> 1] >> 8);
}

static inline void vb_set(u16 *v, int n, u8 b)
{
    n &= 15;
    u16 *e = &v[n >> 1];
    *e = (n & 1) ? (u16)((*e & 0xFF00) | b) : (u16)((*e & 0x00FF) | b << 8);
}

/* --------------------------------------------------------- element selector */
#if defined(__ARM_NEON)
#define B2(n) (2*(n)), (2*(n)+1)
#define S8(a,b,c,d,e,f,g,h) {B2(a),B2(b),B2(c),B2(d),B2(e),B2(f),B2(g),B2(h)}
static const u8 elem_shuf[16][16] __attribute__((aligned(16))) = {
    S8(0,1,2,3,4,5,6,7), S8(0,1,2,3,4,5,6,7),
    S8(0,0,2,2,4,4,6,6), S8(1,1,3,3,5,5,7,7),
    S8(0,0,0,0,4,4,4,4), S8(1,1,1,1,5,5,5,5), S8(2,2,2,2,6,6,6,6), S8(3,3,3,3,7,7,7,7),
    S8(0,0,0,0,0,0,0,0), S8(1,1,1,1,1,1,1,1), S8(2,2,2,2,2,2,2,2), S8(3,3,3,3,3,3,3,3),
    S8(4,4,4,4,4,4,4,4), S8(5,5,5,5,5,5,5,5), S8(6,6,6,6,6,6,6,6), S8(7,7,7,7,7,7,7,7),
};
#else
static const u8 elem_map[16][8] = {
    {0,1,2,3,4,5,6,7}, {0,1,2,3,4,5,6,7},
    {0,0,2,2,4,4,6,6}, {1,1,3,3,5,5,7,7},
    {0,0,0,0,4,4,4,4}, {1,1,1,1,5,5,5,5}, {2,2,2,2,6,6,6,6}, {3,3,3,3,7,7,7,7},
    {0,0,0,0,0,0,0,0}, {1,1,1,1,1,1,1,1}, {2,2,2,2,2,2,2,2}, {3,3,3,3,3,3,3,3},
    {4,4,4,4,4,4,4,4}, {5,5,5,5,5,5,5,5}, {6,6,6,6,6,6,6,6}, {7,7,7,7,7,7,7,7},
};
#endif

static inline void get_vte(u16 *restrict t, int vt, int e)
{
#if defined(__ARM_NEON)
    uint8x16_t v = vreinterpretq_u8_u16(vld1q_u16(R.vr[vt]));
    vst1q_u16(t, vreinterpretq_u16_u8(vqtbl1q_u8(v, vld1q_u8(elem_shuf[e]))));
#else
    const u8 *m = elem_map[e];
    for (int i = 0; i < 8; i++) t[i] = R.vr[vt][m[i]];
#endif
}

/* ---------------------------------------------------------------- clamping */
static inline s32 sclamp(s32 x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }

/* Accumulator bits 47..16 as a signed value. */
static inline s32 acc_hm(int i) { return (s32)((u32)R.acch[i] << 16 | R.accm[i]); }

static inline void set_hm(int i, s32 hm)
{
    R.accm[i] = (u16)hm;
    R.acch[i] = (u16)(hm >> 16);
}

/* Unsigned clamp of the mid slice. */
static inline u16 uclamp_mid(s32 hm) { return (u16)(hm < 0 ? 0 : hm > 32767 ? 65535 : hm); }

/* Low slice, passed through only while bits 47..16 fit in 16 signed bits. */
static inline u16 clamp_low(s32 hm, u32 low)
{
    return (u16)(hm < -32768 ? 0 : hm > 32767 ? 65535 : (s32)(low & 0xFFFF));
}

static inline s32 mul_ss(u16 a, u16 b) { return (s32)(s16)a * (s32)(s16)b; }

/* ---------------------------------------------------------- multiply group */
#define VLOAD                                  \
    u16 s[8], t[8], r[8];                      \
    memcpy(s, R.vr[vs], 16);                   \
    get_vte(t, vt, e)
#define VSTORE memcpy(R.vr[vd], r, 16)

static inline void v_mulf(int vd, int vs, int vt, int e, int uns)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = mul_ss(s[i], t[i]);
        u32 l = ((u32)p << 1 & 0xFFFF) + 0x8000;
        s32 hm = (p >> 15) + (s32)(l >> 16);
        R.accl[i] = (u16)l;
        R.accm[i] = (u16)hm;
        R.acch[i] = (u16)(hm >> 16);
        r[i] = uns ? uclamp_mid(hm) : (u16)sclamp(hm);
    }
    VSTORE;
}

static inline void v_macf(int vd, int vs, int vt, int e, int uns)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = mul_ss(s[i], t[i]);
        u32 l = R.accl[i] + ((u32)p << 1 & 0xFFFF);
        s32 hm = (s32)((u32)acc_hm(i) + (u32)(p >> 15) + (l >> 16));
        R.accl[i] = (u16)l;
        R.accm[i] = (u16)hm;
        R.acch[i] = (u16)(hm >> 16);
        r[i] = uns ? uclamp_mid(hm) : (u16)sclamp(hm);
    }
    VSTORE;
}

/* VMUDL/VMADL keep only the top half of the unsigned product, in the low
 * accumulator slice: that is what lines the 32-bit matrix chain up with
 * VMUDM/VMUDN/VMUDH, which use the accumulator at full width */
static inline void v_mudl(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        u32 p = ((u32)s[i] * t[i]) >> 16;
        R.accl[i] = (u16)p;
        R.accm[i] = 0;
        R.acch[i] = 0;
        r[i] = (u16)p;
    }
    VSTORE;
}

static inline void v_madl(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        u32 l = R.accl[i] + (((u32)s[i] * t[i]) >> 16);
        s32 hm = (s32)((u32)acc_hm(i) + (l >> 16));
        R.accl[i] = (u16)l;
        R.accm[i] = (u16)hm;
        R.acch[i] = (u16)(hm >> 16);
        r[i] = clamp_low(hm, l);
    }
    VSTORE;
}

/* signed s * unsigned t */
static inline void v_mudm(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = (s32)(s16)s[i] * (s32)t[i];
        R.accl[i] = (u16)p;
        R.accm[i] = (u16)(p >> 16);
        R.acch[i] = (u16)(p >> 31);
        r[i] = (u16)(p >> 16);
    }
    VSTORE;
}

static inline void v_madm(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = (s32)(s16)s[i] * (s32)t[i];
        u32 l = R.accl[i] + ((u32)p & 0xFFFF);
        s32 hm = (s32)((u32)acc_hm(i) + (u32)(p >> 16) + (l >> 16));
        R.accl[i] = (u16)l;
        R.accm[i] = (u16)hm;
        R.acch[i] = (u16)(hm >> 16);
        r[i] = (u16)sclamp(hm);
    }
    VSTORE;
}

/* unsigned s * signed t */
static inline void v_mudn(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = (s32)s[i] * (s32)(s16)t[i];
        R.accl[i] = (u16)p;
        R.accm[i] = (u16)(p >> 16);
        R.acch[i] = (u16)(p >> 31);
        r[i] = clamp_low(p >> 16, (u32)p);
    }
    VSTORE;
}

static inline void v_madn(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = (s32)s[i] * (s32)(s16)t[i];
        u32 l = R.accl[i] + ((u32)p & 0xFFFF);
        s32 hm = (s32)((u32)acc_hm(i) + (u32)(p >> 16) + (l >> 16));
        R.accl[i] = (u16)l;
        R.accm[i] = (u16)hm;
        R.acch[i] = (u16)(hm >> 16);
        r[i] = clamp_low(hm, l);
    }
    VSTORE;
}

static inline void v_mudh(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = mul_ss(s[i], t[i]);
        R.accl[i] = 0;
        R.accm[i] = (u16)p;
        R.acch[i] = (u16)(p >> 16);
        r[i] = (u16)sclamp(p);
    }
    VSTORE;
}

static inline void v_madh(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 hm = (s32)((u32)acc_hm(i) + (u32)mul_ss(s[i], t[i]));
        R.accm[i] = (u16)hm;
        R.acch[i] = (u16)(hm >> 16);
        r[i] = (u16)sclamp(hm);
    }
    VSTORE;
}

static void v_mulq(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 p = mul_ss(s[i], t[i]);
        if (p < 0) p += 31;
        R.accl[i] = 0;
        set_hm(i, p);
        r[i] = (u16)(sclamp(p >> 1) & ~15);
    }
    VSTORE;
}

static void v_macq(int vd)
{
    u16 r[8];
    for (int i = 0; i < 8; i++) {
        s32 p = acc_hm(i);
        if (!(p & 32)) {
            if (p < 0) p += 32;
            else if (p >= 32) p -= 32;
        }
        set_hm(i, p);
        r[i] = (u16)(sclamp(p >> 1) & ~15);
    }
    memcpy(R.vr[vd], r, 16);
}

static void v_rnd(int vd, int vs, int vt, int e, int positive)
{
    u16 t[8], r[8];
    get_vte(t, vt, e);
    for (int i = 0; i < 8; i++) {
        s64 add = (s16)t[i];
        if (vs & 1) add *= 65536;
        /* Sign extend the 48-bit accumulator. */
        u64 raw = (u64)R.acch[i] << 32 | (u64)R.accm[i] << 16 | R.accl[i];
        s64 acc = (s64)(raw << 16) / 65536;
        if (positive ? acc >= 0 : acc < 0) acc += add;
        R.accl[i] = (u16)acc;
        set_hm(i, (s32)((u64)acc >> 16));
        r[i] = (u16)sclamp(acc_hm(i));
    }
    memcpy(R.vr[vd], r, 16);
}

/* ---------------------------------------------------------- add/sub/select */
static inline void clear_vco(void)
{
    memset(R.vcol, 0, 16);
    memset(R.vcoh, 0, 16);
}

static inline void v_add(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 a = (s32)(s16)s[i] + (s16)t[i] + R.vcol[i];
        R.accl[i] = (u16)a;
        r[i] = (u16)sclamp(a);
    }
    clear_vco();
    VSTORE;
}

static inline void v_sub(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 a = (s32)(s16)s[i] - (s16)t[i] - R.vcol[i];
        R.accl[i] = (u16)a;
        r[i] = (u16)sclamp(a);
    }
    clear_vco();
    VSTORE;
}

static inline void v_abs(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 sv = (s16)s[i], tv = (s16)t[i];
        s32 a = sv < 0 ? -tv : sv > 0 ? tv : 0;
        R.accl[i] = (u16)a;
        r[i] = (u16)sclamp(a);
    }
    VSTORE;
}

static inline void v_addc(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        u32 a = (u32)s[i] + t[i];
        r[i] = R.accl[i] = (u16)a;
        R.vcol[i] = (u16)(a >> 16);
        R.vcoh[i] = 0;
    }
    VSTORE;
}

static inline void v_subc(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 a = (s32)s[i] - (s32)t[i];
        r[i] = R.accl[i] = (u16)a;
        R.vcol[i] = a < 0;
        R.vcoh[i] = a != 0;
    }
    VSTORE;
}

#define V_LOGIC(name, expr)                                  \
    static inline void name(int vd, int vs, int vt, int e)   \
    {                                                        \
        VLOAD;                                               \
        for (int i = 0; i < 8; i++) {                        \
            r[i] = (u16)(expr);                              \
            R.accl[i] = r[i];                                \
        }                                                    \
        VSTORE;                                              \
    }
V_LOGIC(v_and,  s[i] & t[i])
V_LOGIC(v_nand, ~(s[i] & t[i]))
V_LOGIC(v_or,   s[i] | t[i])
V_LOGIC(v_nor,  ~(s[i] | t[i]))
V_LOGIC(v_xor,  s[i] ^ t[i])
V_LOGIC(v_nxor, ~(s[i] ^ t[i]))

/* VCCH, VCOL and VCOH end up clear. */
#define V_CMP(name, cond)                                    \
    static inline void name(int vd, int vs, int vt, int e)   \
    {                                                        \
        VLOAD;                                               \
        for (int i = 0; i < 8; i++) {                        \
            s32 a = (s16)s[i], b = (s16)t[i];                \
            u16 c = (cond) ? 1 : 0;                          \
            R.vccl[i] = c;                                   \
            r[i] = c ? s[i] : t[i];                          \
            R.accl[i] = r[i];                                \
        }                                                    \
        memset(R.vcch, 0, 16);                               \
        clear_vco();                                         \
        VSTORE;                                              \
    }
V_CMP(v_lt, a < b || (a == b && R.vcol[i] && R.vcoh[i]))
V_CMP(v_eq, a == b && !R.vcoh[i])
V_CMP(v_ne, a != b || R.vcoh[i])
V_CMP(v_ge, a > b || (a == b && !(R.vcol[i] && R.vcoh[i])))

static inline void v_cl(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        u16 sv = s[i], tv = t[i], a;
        if (R.vcol[i]) {
            if (!R.vcoh[i]) {
                u32 total = (u32)sv + tv;
                int zero = (total & 0xFFFF) == 0, carry = total > 0xFFFF;
                R.vccl[i] = R.vce[i] ? (zero || !carry) : (zero && !carry);
            }
            a = R.vccl[i] ? (u16)-tv : sv;
        } else {
            if (!R.vcoh[i]) R.vcch[i] = sv >= tv;
            a = R.vcch[i] ? tv : sv;
        }
        R.accl[i] = r[i] = a;
    }
    clear_vco();
    memset(R.vce, 0, 16);
    VSTORE;
}

static inline void v_ch(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 a = (s16)s[i], b = (s16)t[i];
        int ne_comp = s[i] != (u16)~t[i];
        if ((a ^ b) < 0) {
            s32 sum = a + b;
            int le = sum <= 0;
            R.accl[i] = r[i] = (u16)(le ? -b : a);
            R.vccl[i] = (u16)le;
            R.vcch[i] = b < 0;
            R.vcol[i] = 1;
            R.vcoh[i] = sum != 0 && ne_comp;
            R.vce[i] = sum == -1;
        } else {
            s32 diff = a - b;
            int ge = diff >= 0;
            R.accl[i] = r[i] = (u16)(ge ? b : a);
            R.vccl[i] = b < 0;
            R.vcch[i] = (u16)ge;
            R.vcol[i] = 0;
            R.vcoh[i] = diff != 0 && ne_comp;
            R.vce[i] = 0;
        }
    }
    VSTORE;
}

static inline void v_cr(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        s32 a = (s16)s[i], b = (s16)t[i];
        if ((a ^ b) < 0) {
            int le = a + b + 1 <= 0;
            R.vcch[i] = b < 0;
            R.vccl[i] = (u16)le;
            R.accl[i] = r[i] = le ? (u16)~t[i] : s[i];
        } else {
            int ge = a - b >= 0;
            R.vccl[i] = b < 0;
            R.vcch[i] = (u16)ge;
            R.accl[i] = r[i] = ge ? t[i] : s[i];
        }
    }
    clear_vco();
    memset(R.vce, 0, 16);
    VSTORE;
}

static inline void v_mrg(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        r[i] = R.vccl[i] ? s[i] : t[i];
        R.accl[i] = r[i];
    }
    clear_vco();
    VSTORE;
}

static inline void v_sar(int vd, int e)
{
    switch (e) {
    case 8:  memcpy(R.vr[vd], R.acch, 16); break;
    case 9:  memcpy(R.vr[vd], R.accm, 16); break;
    case 10: memcpy(R.vr[vd], R.accl, 16); break;
    default: memset(R.vr[vd], 0, 16); break;
    }
}

/* Reserved encodings still add into the low accumulator; the result is zero. */
static void v_zero(int vd, int vs, int vt, int e)
{
    VLOAD;
    for (int i = 0; i < 8; i++) {
        R.accl[i] = (u16)(s[i] + t[i]);
        r[i] = 0;
    }
    VSTORE;
}

/* ------------------------------------------------------------ single lane */
static inline void v_mov(int vd, int de, int vt, int e)
{
    u16 t[8];
    get_vte(t, vt, e);
    R.vr[vd][de] = t[de];
    memcpy(R.accl, t, 16);
}

/* Shared body of VRCP(L) and VRSQ(L). */
static void v_div(int vd, int de, int vt, int e, int dbl, int root)
{
    u16 t[8];
    u16 lane = R.vr[vt][e & 7];
    s32 input, result;
    if (dbl && R.div_dp) input = (s32)((u32)R.div_in << 16 | lane);
    else input = (s16)lane;

    s32 mask = input >> 31;
    s32 data = input ^ mask;
    if (input > -32768) data -= mask;

    if (data == 0) {
        result = 0x7FFFFFFF;
    } else if (input == -32768) {
        result = (s32)0xFFFF0000;
    } else {
        int shift = __builtin_clz((u32)data);
        u32 index = (u32)(((u64)(u32)data << shift) & 0x7FC00000) >> 22;
        s32 scaled;
        if (root) {
            scaled = (s32)((0x10000u | rsq_rom[(index & 0x1FE) | (u32)(shift & 1)]) << 14);
            result = (scaled >> ((31 - shift) / 2)) ^ mask;
        } else {
            scaled = (s32)((0x10000u | rcp_rom[index]) << 14);
            result = (scaled >> (31 - shift)) ^ mask;
        }
    }
    R.div_dp = 0;
    R.div_out = (u16)((u32)result >> 16);
    get_vte(t, vt, e);
    memcpy(R.accl, t, 16);
    R.vr[vd][de] = (u16)result;
}

static void v_divh(int vd, int de, int vt, int e)
{
    u16 t[8];
    get_vte(t, vt, e);
    memcpy(R.accl, t, 16);
    R.div_dp = 1;
    R.div_in = R.vr[vt][e & 7];
    R.vr[vd][de] = R.div_out;
}

/* ------------------------------------------------------------ COP2 dispatch */
static void cop2_compute(u32 op)
{
    int e  = (op >> 21) & 15;
    int vt = (op >> 16) & 31;
    int vs = (op >> 11) & 31;
    int vd = (op >> 6) & 31;

    switch (op & 63) {
    case 0x00: v_mulf(vd, vs, vt, e, 0); break;
    case 0x01: v_mulf(vd, vs, vt, e, 1); break;
    case 0x02: v_rnd(vd, vs, vt, e, 1); break;
    case 0x03: v_mulq(vd, vs, vt, e); break;
    case 0x04: v_mudl(vd, vs, vt, e); break;
    case 0x05: v_mudm(vd, vs, vt, e); break;
    case 0x06: v_mudn(vd, vs, vt, e); break;
    case 0x07: v_mudh(vd, vs, vt, e); break;
    case 0x08: v_macf(vd, vs, vt, e, 0); break;
    case 0x09: v_macf(vd, vs, vt, e, 1); break;
    case 0x0A: v_rnd(vd, vs, vt, e, 0); break;
    case 0x0B: v_macq(vd); break;
    case 0x0C: v_madl(vd, vs, vt, e); break;
    case 0x0D: v_madm(vd, vs, vt, e); break;
    case 0x0E: v_madn(vd, vs, vt, e); break;
    case 0x0F: v_madh(vd, vs, vt, e); break;
    case 0x10: v_add(vd, vs, vt, e); break;
    case 0x11: v_sub(vd, vs, vt, e); break;
    case 0x13: v_abs(vd, vs, vt, e); break;
    case 0x14: v_addc(vd, vs, vt, e); break;
    case 0x15: v_subc(vd, vs, vt, e); break;
    case 0x1D: v_sar(vd, e); break;
    case 0x20: v_lt(vd, vs, vt, e); break;
    case 0x21: v_eq(vd, vs, vt, e); break;
    case 0x22: v_ne(vd, vs, vt, e); break;
    case 0x23: v_ge(vd, vs, vt, e); break;
    case 0x24: v_cl(vd, vs, vt, e); break;
    case 0x25: v_ch(vd, vs, vt, e); break;
    case 0x26: v_cr(vd, vs, vt, e); break;
    case 0x27: v_mrg(vd, vs, vt, e); break;
    case 0x28: v_and(vd, vs, vt, e); break;
    case 0x29: v_nand(vd, vs, vt, e); break;
    case 0x2A: v_or(vd, vs, vt, e); break;
    case 0x2B: v_nor(vd, vs, vt, e); break;
    case 0x2C: v_xor(vd, vs, vt, e); break;
    case 0x2D: v_nxor(vd, vs, vt, e); break;
    case 0x30: v_div(vd, vs & 7, vt, e, 0, 0); break;   /* VRCP */
    case 0x31: v_div(vd, vs & 7, vt, e, 1, 0); break;   /* VRCPL */
    case 0x32: v_divh(vd, vs & 7, vt, e); break;        /* VRCPH */
    case 0x33: v_mov(vd, vs & 7, vt, e); break;
    case 0x34: v_div(vd, vs & 7, vt, e, 0, 1); break;   /* VRSQ */
    case 0x35: v_div(vd, vs & 7, vt, e, 1, 1); break;   /* VRSQL */
    case 0x36: v_divh(vd, vs & 7, vt, e); break;        /* VRSQH */
    case 0x37: case 0x3F: break;                        /* VNOP, VNULL */
    default:   v_zero(vd, vs, vt, e); break;
    }
}

static u32 flags_pack(const u16 *lo, const u16 *hi)
{
    u32 v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (u32)(lo[i] & 1) << i;
        if (hi) v |= (u32)(hi[i] & 1) << (i + 8);
    }
    return v;
}

static void flags_unpack(u32 v, u16 *lo, u16 *hi)
{
    for (int i = 0; i < 8; i++) {
        lo[i] = (v >> i) & 1;
        if (hi) hi[i] = (v >> (i + 8)) & 1;
    }
}

static inline u32 cfc2(int rd)
{
    u32 v;
    switch (rd & 3) {
    case 0:  v = flags_pack(R.vcol, R.vcoh); break;
    case 1:  v = flags_pack(R.vccl, R.vcch); break;
    default: v = flags_pack(R.vce, NULL); break;
    }
    return (u32)(s32)(s16)v;
}

static inline void ctc2(int rd, u32 v)
{
    switch (rd & 3) {
    case 0:  flags_unpack(v, R.vcol, R.vcoh); break;
    case 1:  flags_unpack(v, R.vccl, R.vcch); break;
    default: flags_unpack(v, R.vce, NULL); break;
    }
}

/* ---------------------------------------------------------- vector loads
 * Addressing and wrapping follow hardware: most formats stop at the register
 * or at a 16-byte DMEM line rather than wrapping, the packed ones index a
 * rotated line, and each aliases differently for odd elements. */
static inline void lwc2(u32 op)
{
    int base = (op >> 21) & 31;
    int vt = (op >> 16) & 31;
    int fmt = (op >> 11) & 31;
    int e = (op >> 7) & 15;
    int off = (int)((op & 0x7F) ^ 0x40) - 0x40;
    u32 ea = R.gpr[base];
    u16 *v = R.vr[vt];

    switch (fmt) {
    case 0x00:                                 /* LBV */
        vb_set(v, e, dm_r8(ea + (u32)off));
        break;
    case 0x01: case 0x02: case 0x03: {         /* LSV LLV LDV */
        int size = 1 << fmt, end = e + size;
        ea += (u32)(off * size);
        if (end > 16) end = 16;
        for (int i = e; i < end; i++) vb_set(v, i, dm_r8(ea++));
        break;
    }
    case 0x04: {                               /* LQV */
        ea += (u32)(off * 16);
#if defined(__ARM_NEON)
        if (e == 0 && (ea & 15) == 0 && (ea & 0xFFF) <= 0xFF0) {
            uint8x16_t d = vrev16q_u8(vld1q_u8(dmem + (ea & 0xFFF)));
            vst1q_u16(v, vreinterpretq_u16_u8(d));
            break;
        }
#endif
        int end = 16 + e - (int)(ea & 15);
        if (end > 16) end = 16;
        for (int i = e; i < end; i++) vb_set(v, i, dm_r8(ea++));
        break;
    }
    case 0x05: {                               /* LRV */
        ea += (u32)(off * 16);
        int start = 16 - (int)(ea & 15) + e;
        ea &= ~15u;
        for (int i = start; i < 16; i++) vb_set(v, i, dm_r8(ea++));
        break;
    }
    case 0x06: case 0x07: {                    /* LPV LUV */
        int shift = fmt == 6 ? 8 : 7;
        ea += (u32)(off * 8);
        int index = (int)(ea & 7) - e;
        ea &= ~7u;
        for (int i = 0; i < 8; i++)
            v[i] = (u16)(dm_r8(ea + (u32)((index + i) & 15)) << shift);
        break;
    }
    case 0x08: {                               /* LHV */
        ea += (u32)(off * 16);
        int index = (int)(ea & 15) - e;
        ea &= ~15u;
        for (int i = 0; i < 8; i++)
            v[i] = (u16)(dm_r8(ea + (u32)((index + i * 2) & 15)) << 7);
        break;
    }
    case 0x09: {                               /* LFV */
        u16 tmp[8];
        ea += (u32)(off * 16);
        int index = (int)(ea & 7) - e;
        ea &= ~7u;
        for (int i = 0; i < 4; i++) {
            tmp[i]     = (u16)(dm_r8(ea + (u32)((index + i * 4) & 15)) << 7);
            tmp[i + 4] = (u16)(dm_r8(ea + (u32)((index + i * 4 + 8) & 15)) << 7);
        }
        int end = e + 8 > 16 ? 16 : e + 8;
        for (int i = e; i < end; i++) vb_set(v, i, vb_get(tmp, i));
        break;
    }
    case 0x0B: {                               /* LTV */
        ea += (u32)(off * 16);
        int first = vt & ~7;
        ea = ((ea + 8) & ~15u) + (u32)(e & 1);
        for (int i = 0; i < 8; i++) {
            int b = (8 - (e >> 1) + i) << 1;
            vb_set(R.vr[first + i], b, dm_r8(ea++));
            vb_set(R.vr[first + i], b + 1, dm_r8(ea++));
        }
        break;
    }
    default:                                   /* LWV and reserved */
        break;
    }
}

/* ---------------------------------------------------------- vector stores */
static inline void swc2(u32 op)
{
    int base = (op >> 21) & 31;
    int vt = (op >> 16) & 31;
    int fmt = (op >> 11) & 31;
    int e = (op >> 7) & 15;
    int off = (int)((op & 0x7F) ^ 0x40) - 0x40;
    u32 ea = R.gpr[base];
    const u16 *v = R.vr[vt];

    switch (fmt) {
    case 0x00:                                 /* SBV */
        dm_w8(ea + (u32)off, vb_get(v, e));
        break;
    case 0x01: case 0x02: case 0x03: {         /* SSV SLV SDV */
        int size = 1 << fmt;
        ea += (u32)(off * size);
        for (int i = e; i < e + size; i++) dm_w8(ea++, vb_get(v, i));
        break;
    }
    case 0x04: {                               /* SQV */
        ea += (u32)(off * 16);
#if defined(__ARM_NEON)
        if (e == 0 && (ea & 15) == 0 && (ea & 0xFFF) <= 0xFF0) {
            uint8x16_t d = vreinterpretq_u8_u16(vld1q_u16(v));
            vst1q_u8(dmem + (ea & 0xFFF), vrev16q_u8(d));
            break;
        }
#endif
        int end = e + 16 - (int)(ea & 15);
        for (int i = e; i < end; i++) dm_w8(ea++, vb_get(v, i));
        break;
    }
    case 0x05: {                               /* SRV */
        ea += (u32)(off * 16);
        int count = (int)(ea & 15), shift = 16 - count;
        ea &= ~15u;
        for (int i = e; i < e + count; i++) dm_w8(ea++, vb_get(v, i + shift));
        break;
    }
    case 0x06: case 0x07: {                    /* SPV SUV */
        int split = fmt == 6 ? 0 : 8;          /* where the formats swap */
        ea += (u32)(off * 8);
        for (int i = e; i < e + 8; i++) {
            int high = ((i & 15) < 8) ^ (split != 0);
            if (high) dm_w8(ea++, vb_get(v, (i & 7) << 1));
            else      dm_w8(ea++, (u8)(v[i & 7] >> 7));
        }
        break;
    }
    case 0x08: {                               /* SHV */
        ea += (u32)(off * 16);
        int index = (int)(ea & 7);
        ea &= ~7u;
        for (int i = 0; i < 8; i++) {
            int b = e + i * 2;
            u8 value = (u8)(vb_get(v, b) << 1 | vb_get(v, b + 1) >> 7);
            dm_w8(ea + (u32)((index + i * 2) & 15), value);
        }
        break;
    }
    case 0x09: {                               /* SFV */
        static const s8 map[16][4] = {
            [0]  = {0, 1, 2, 3}, [15] = {0, 1, 2, 3},
            [1]  = {6, 7, 4, 5}, [4]  = {1, 2, 3, 0},
            [5]  = {7, 4, 5, 6}, [8]  = {4, 5, 6, 7},
            [11] = {3, 0, 1, 2}, [12] = {5, 6, 7, 4},
            [2]  = {-1,-1,-1,-1}, [3]  = {-1,-1,-1,-1},
            [6]  = {-1,-1,-1,-1}, [7]  = {-1,-1,-1,-1},
            [9]  = {-1,-1,-1,-1}, [10] = {-1,-1,-1,-1},
            [13] = {-1,-1,-1,-1}, [14] = {-1,-1,-1,-1},
        };
        ea += (u32)(off * 16);
        int index = (int)(ea & 7);
        ea &= ~7u;
        for (int i = 0; i < 4; i++) {
            int el = map[e][i];
            dm_w8(ea + (u32)((index + i * 4) & 15), el < 0 ? 0 : (u8)(v[el] >> 7));
        }
        break;
    }
    case 0x0A: {                               /* SWV */
        ea += (u32)(off * 16);
        int index = (int)(ea & 7);
        ea &= ~7u;
        for (int i = 0; i < 16; i++)
            dm_w8(ea + (u32)((index + i) & 15), vb_get(v, e + i));
        break;
    }
    case 0x0B: {                               /* STV */
        ea += (u32)(off * 16);
        int first = vt & ~7;
        int b = 16 - (e & ~1);
        int index = (int)(ea & 7) - (e & ~1);
        ea &= ~7u;
        for (int i = 0; i < 8; i++) {
            dm_w8(ea + (u32)(index++ & 15), vb_get(R.vr[first + i], b++));
            dm_w8(ea + (u32)(index++ & 15), vb_get(R.vr[first + i], b++));
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- interface */
void rsp_reset(void)
{
    memset(&R, 0, sizeof R);
}

void rsp_set_pc(u32 pc)
{
    R.pc = pc & 0xFFC;
    R.delay = 0;
}

u32 rsp_get_pc(void)
{
    return R.pc;
}

#define BRANCH(addr) do { R.target = (addr) & 0xFFC; R.delay = 1; } while (0)

int rsp_run(int cycles)
{
    if (cycles <= 0 || n64_sp_halted()) return 0;

    const u8 *imem = n64_sp_mem + 0x1000;
    u32 *g = R.gpr;
    u32 pc = R.pc;
    int used = 0;

    while (used < cycles) {
        u32 cur = pc, op;
        memcpy(&op, imem + cur, 4);
        op = __builtin_bswap32(op);

        if (R.delay) {
            pc = R.target;
            R.delay = 0;
        } else {
            pc = (cur + 4) & 0xFFC;
        }
        used++;

        int rs = (op >> 21) & 31, rt = (op >> 16) & 31;
        int rd = (op >> 11) & 31, sa = (op >> 6) & 31;
        u32 imm = (u32)(s32)(s16)op;
        u32 ea = g[rs] + imm;
        u32 btarget = cur + 4 + (imm << 2);

        switch (op >> 26) {
        case 0x00:
            switch (op & 63) {
            case 0x00: g[rd] = g[rt] << sa; break;
            case 0x02: g[rd] = g[rt] >> sa; break;
            case 0x03: g[rd] = (u32)((s32)g[rt] >> sa); break;
            case 0x04: g[rd] = g[rt] << (g[rs] & 31); break;
            case 0x06: g[rd] = g[rt] >> (g[rs] & 31); break;
            case 0x07: g[rd] = (u32)((s32)g[rt] >> (g[rs] & 31)); break;
            case 0x08: BRANCH(g[rs]); break;
            case 0x09: {
                u32 to = g[rs];
                g[rd] = (cur + 8) & 0xFFF;
                BRANCH(to);
                break;
            }
            case 0x0D:
                n64_sp_break();
                goto out;
            case 0x20: case 0x21: g[rd] = g[rs] + g[rt]; break;
            case 0x22: case 0x23: g[rd] = g[rs] - g[rt]; break;
            case 0x24: g[rd] = g[rs] & g[rt]; break;
            case 0x25: g[rd] = g[rs] | g[rt]; break;
            case 0x26: g[rd] = g[rs] ^ g[rt]; break;
            case 0x27: g[rd] = ~(g[rs] | g[rt]); break;
            case 0x2A: g[rd] = (s32)g[rs] < (s32)g[rt]; break;
            case 0x2B: g[rd] = g[rs] < g[rt]; break;
            default: break;
            }
            break;
        case 0x01: {
            int taken = (s32)g[rs] < 0;
            switch (rt) {
            case 0x00: break;
            case 0x01: taken = !taken; break;
            case 0x10: g[31] = (cur + 8) & 0xFFF; break;
            case 0x11: taken = !taken; g[31] = (cur + 8) & 0xFFF; break;
            default: taken = 0; break;
            }
            if (taken) BRANCH(btarget);
            break;
        }
        case 0x03:
            g[31] = (cur + 8) & 0xFFF;
            /* fall through */
        case 0x02:
            BRANCH(op << 2);
            break;
        case 0x04: if (g[rs] == g[rt]) BRANCH(btarget); break;
        case 0x05: if (g[rs] != g[rt]) BRANCH(btarget); break;
        case 0x06: if ((s32)g[rs] <= 0) BRANCH(btarget); break;
        case 0x07: if ((s32)g[rs] > 0) BRANCH(btarget); break;
        case 0x08: case 0x09: g[rt] = ea; break;
        case 0x0A: g[rt] = (s32)g[rs] < (s32)imm; break;
        case 0x0B: g[rt] = g[rs] < imm; break;
        case 0x0C: g[rt] = g[rs] & (op & 0xFFFF); break;
        case 0x0D: g[rt] = g[rs] | (op & 0xFFFF); break;
        case 0x0E: g[rt] = g[rs] ^ (op & 0xFFFF); break;
        case 0x0F: g[rt] = op << 16; break;
        case 0x10:
            if (rs == 0) {
                g[rt] = n64_rsp_cop0_read(rd & 15);
            } else if (rs == 4) {
                n64_rsp_cop0_write(rd & 15, g[rt]);
                if (n64_sp_halted()) goto out;
            }
            break;
        case 0x12:
            if (rs & 16) {
                cop2_compute(op);
            } else {
                int el = (op >> 7) & 15;
                u16 *v = R.vr[rd];
                switch (rs) {
                case 0x00:
                    g[rt] = (u32)(s32)(s16)(vb_get(v, el) << 8 | vb_get(v, el + 1));
                    break;
                case 0x02: g[rt] = cfc2(rd); break;
                case 0x04:
                    vb_set(v, el, (u8)(g[rt] >> 8));
                    if (el != 15) vb_set(v, el + 1, (u8)g[rt]);
                    break;
                case 0x06: ctc2(rd, g[rt]); break;
                default: break;
                }
            }
            break;
        case 0x20: g[rt] = (u32)(s32)(s8)dm_r8(ea); break;
        case 0x21: g[rt] = (u32)(s32)(s16)dm_r16(ea); break;
        case 0x23: case 0x27: g[rt] = dm_r32(ea); break;
        case 0x24: g[rt] = dm_r8(ea); break;
        case 0x25: g[rt] = dm_r16(ea); break;
        case 0x28: dm_w8(ea, (u8)g[rt]); break;
        case 0x29: dm_w16(ea, g[rt]); break;
        case 0x2B: dm_w32(ea, g[rt]); break;
        case 0x32: lwc2(op); break;
        case 0x3A: swc2(op); break;
        default: break;
        }
        g[0] = 0;
    }
out:
    g[0] = 0;
    R.pc = pc;
    return used;
}

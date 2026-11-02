/*
 * psp_vfpu.c - the Allegrex vector unit, one instruction at a time.
 *
 * 128 floats, seen as 8 matrices of 4x4 with columns and rows picked by the
 * register number. Source and destination prefixes (swizzle, negate, constants,
 * saturate, mask) are set by the vpfx instructions and used up by the next
 * vector instruction.
 */
#include <stdio.h>
#include "allegrex.h"

#define PI 3.14159265358979323846f
enum { S_PREFIX, T_PREFIX, D_PREFIX };
typedef union { float f; u32 w; s32 i; } V;

u64 psp_vfpu_unknown;
u32 psp_vfpu_missing[64]; int psp_vfpu_nmissing;

static void missing(u32 inst) {
    psp_vfpu_unknown++;
    if (psp_vfpu_nmissing < 64) {
        for (int i = 0; i < psp_vfpu_nmissing; i++) if ((psp_vfpu_missing[i] ^ inst) >> 16 == 0) return;
        psp_vfpu_missing[psp_vfpu_nmissing++] = inst;
    }
}
static u32 vget(int r) { u32 v; memcpy(&v, &p.v[r], 4); return v; }
static void vset(int r, u32 v) { memcpy(&p.v[r], &v, 4); }

static void vec_regs(u8 *regs, int n, int reg) {
    const int m = (reg >> 2) & 7, col = reg & 3;
    int row, tr = (reg >> 5) & 1;
    switch (n) {
    case 1: tr = 0; row = (reg >> 5) & 3; break;
    case 3: row = (reg >> 6) & 1; break;
    default: row = (reg >> 5) & 2; break;
    }
    for (int i = 0; i < n; i++) {
        const int a = (row + i) & 3;
        regs[i] = m * 4 + (tr ? a + col * 32 : col + a * 32);
    }
}
static void mtx_regs(u8 *regs, int n, int reg) {
    const int m = (reg >> 2) & 7;
    int col, row, tr = (reg >> 5) & 1;
    switch (n) {
    case 1: tr = 0; col = reg & 3; row = (reg >> 5) & 3; break;
    case 3: col = reg & 1; row = (reg >> 6) & 1; break;
    default: col = reg & 2; row = (reg >> 5) & 2; break;
    }
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        const int a = (row + i) & 3, b = (col + j) & 3;
        regs[j * 4 + i] = m * 4 + (tr ? a + b * 32 : b + a * 32);
    }
}
static void rd_raw(V *d, int n, int reg) { u8 r[4]; vec_regs(r, n, reg); for (int i = 0; i < n; i++) d[i].w = vget(r[i]); }
static void rd(V *d, int n, int reg, int which) {
    V o[4]; const u32 x = p.vctl[which];
    static const float k[8] = { 0, 1, 2, 0.5f, 3, 1.f / 3, 0.25f, 1.f / 6 };
    rd_raw(o, n, reg);
    for (int i = 0; i < n; i++) {
        const int reg_i = (x >> (i * 2)) & 3, ab = (x >> (8 + i)) & 1, c = (x >> (12 + i)) & 1, neg = (x >> (16 + i)) & 1;
        if (c) d[i].f = k[reg_i + (ab << 2)];
        else { d[i].w = reg_i < n ? o[reg_i].w : 0; if (ab) d[i].w &= 0x7FFFFFFFu; }
        if (neg) d[i].w ^= 0x80000000u;
    }
}
static void wr(const V *d, int n, int reg) {
    u8 r[4]; const u32 x = p.vctl[D_PREFIX];
    vec_regs(r, n, reg);
    for (int i = 0; i < n; i++) {
        if ((x >> (8 + i)) & 1) continue;
        V v = d[i]; const int sat = (x >> (i * 2)) & 3;
        if (sat == 1) v.f = v.f != v.f ? v.f : v.f <= 0 ? 0 : v.f >= 1 ? 1 : v.f;
        if (sat == 3) v.f = v.f != v.f ? v.f : v.f <= -1 ? -1 : v.f >= 1 ? 1 : v.f;
        vset(r[i], v.w);
    }
}
static void wr_int(const V *d, int n, int reg) {        /* integers skip saturation */
    u8 r[4]; const u32 x = p.vctl[D_PREFIX];
    vec_regs(r, n, reg);
    for (int i = 0; i < n; i++) if (!((x >> (8 + i)) & 1)) vset(r[i], d[i].w);
}
static void prefix_done(void) { p.vctl[S_PREFIX] = p.vctl[T_PREFIX] = 0xE4; p.vctl[D_PREFIX] = 0; }

static float sinq(float a) {                     /* a quarter turn is 1, and exact where the hardware is */
    float m = fmodf(a, 4.f);
    if (m == 0 || m == 2 || m == -2) return 0;
    if (m == 1 || m == -3) return 1;
    if (m == -1 || m == 3) return -1;
    return sinf(m * PI / 2);
}
static float cosq(float a) { return sinq(a + 1); }

static s32 to_int(double v) { return v != v ? 0x7FFFFFFF : v >= 2147483647.0 ? 0x7FFFFFFF : v <= -2147483648.0 ? (s32)0x80000000 : (s32)v; }

static int vcmp(int c, float a, float b) {
    switch (c) {
    case 0: return 0; case 1: return a == b; case 2: return a < b; case 3: return a <= b;
    case 4: return 1; case 5: return a != b; case 6: return a >= b; case 7: return a > b;
    case 8: return a == 0; case 9: return a != a; case 10: return isinf(a); case 11: return a != a || isinf(a);
    case 12: return a != 0; case 13: return a == a; case 14: return !isinf(a); default: return a == a && !isinf(a);
    }
}

void psp_vfpu(u32 op) {
    const u32 major = op >> 26;
    const int vd = op & 0x7F, vs = (op >> 8) & 0x7F, vt = (op >> 16) & 0x7F;
    const int n = 1 + (((op >> 7) & 1) | ((op >> 14) & 2));
    V s[4], t[4], d[4];
    if (major == 18) {                             /* moves to and from the main registers */
        const int rs = (op >> 21) & 31, rt = (op >> 16) & 31, imm = op & 0xFF;
        if (rs == 3) {
            if (rt) p.r[rt] = imm < 128 ? vget(imm) : imm == 131 ? p.vcc : imm < 144 ? p.vctl[imm - 128] : 0;
        } else if (rs == 7) {
            if (imm < 128) vset(imm, p.r[rt]);
            else if (imm == 131) p.vcc = p.r[rt];
            else if (imm < 144) p.vctl[imm - 128] = p.r[rt];
        } else missing(op);
        return;
    }
    if (major == 24 || major == 25 || major == 27) {
        const int ins = (op >> 23) & 7;
        rd(s, n, vs, S_PREFIX); rd(t, n, vt, T_PREFIX);
        if (major == 24) {
            for (int i = 0; i < n; i++) switch (ins) {
            case 0: d[i].f = s[i].f + t[i].f; break;
            case 1: d[i].f = s[i].f - t[i].f; break;
            case 7: d[i].f = s[i].f / t[i].f; break;
            default: missing(op); d[i].f = 0; break;
            }
            wr(d, n, vd);
        } else if (major == 25) {
            float sum = 0; int out = n;
            switch (ins) {
            case 0: for (int i = 0; i < n; i++) d[i].f = s[i].f * t[i].f; break;
            case 1: for (int i = 0; i < n; i++) sum += s[i].f * t[i].f; d[0].f = sum; out = 1; break;
            case 2: rd(t, 1, vt, T_PREFIX); for (int i = 0; i < n; i++) d[i].f = s[i].f * t[0].f; break;
            case 4: for (int i = 0; i < n - 1; i++) sum += s[i].f * t[i].f; d[0].f = sum + t[n - 1].f; out = 1; break;
            case 5: d[0].f = s[1].f * t[2].f; d[1].f = s[2].f * t[0].f; d[2].f = s[0].f * t[1].f; break;
            case 6: d[0].f = s[0].f * t[1].f - s[1].f * t[0].f; out = 1; break;
            default: missing(op); out = 0; break;
            }
            if (out) wr(d, out, vd);
        } else if (ins == 0) {
            int cc = 0, any = 0, all = 1;
            for (int i = 0; i < n; i++) {
                const int c = vcmp(op & 15, s[i].f, t[i].f);
                cc |= c << i; any |= c; all &= c;
            }
            p.vcc = (p.vcc & ~0x3Fu) | (u32)cc | (u32)any << 4 | (u32)all << 5;
        } else {
            for (int i = 0; i < n; i++) switch (ins) {
            case 2: d[i].f = s[i].f < t[i].f ? s[i].f : t[i].f; break;
            case 3: d[i].f = s[i].f > t[i].f ? s[i].f : t[i].f; break;
            case 5: d[i].f = s[i].f > t[i].f ? 1 : s[i].f < t[i].f ? -1 : 0; break;
            case 6: d[i].f = s[i].f >= t[i].f ? 1 : 0; break;
            case 7: d[i].f = s[i].f < t[i].f ? 1 : 0; break;
            default: missing(op); d[i].f = 0; break;
            }
            wr(d, n, vd);
        }
        prefix_done();
        return;
    }
    if (major == 55) {
        switch ((op >> 24) & 3) {
        case 0: p.vctl[S_PREFIX] = op & 0xFFFFF; return;
        case 1: p.vctl[T_PREFIX] = op & 0xFFFFF; return;
        case 2: p.vctl[D_PREFIX] = op & 0xFFFFF; return;
        }
        const int r = (op >> 16) & 0x7F;
        if (op & 0x800000) {                       /* vfim: a half-precision constant */
            const u32 h = op & 0xFFFF, e = (h >> 10) & 31; u32 m = h & 0x3FF; V v;
            if (e == 31) v.w = 0x7F800000u | m << 13;
            else if (e == 0 && m == 0) v.w = 0;
            else if (e == 0) { int sh = 0; while (!(m & 0x400)) { m <<= 1; sh++; } v.w = (u32)(113 - sh) << 23 | (m & 0x3FF) << 13; }
            else v.w = (e + 112) << 23 | m << 13;
            v.w |= (h & 0x8000) << 16;
            d[0] = v;
        } else d[0].f = (float)(s16)op;
        wr(d, 1, r);
        prefix_done();
        return;
    }
    if (major == 52) {
        const int group = (op >> 21) & 31, sub = (op >> 16) & 31;
        if (group == 0) {
            if (sub == 3 || sub == 6 || sub == 7) {
                for (int i = 0; i < n; i++) d[i].f = sub == 3 ? (i == (vd & (n - 1)) ? 1 : 0) : sub == 7;
                wr(d, n, vd); prefix_done(); return;
            }
            rd(s, n, vs, S_PREFIX);
            for (int i = 0; i < n; i++) {
                const float a = s[i].f;
                switch (sub) {
                case 0: d[i] = s[i]; break;
                case 1: d[i].w = s[i].w & 0x7FFFFFFFu; break;
                case 2: d[i].w = s[i].w ^ 0x80000000u; break;
                case 4: d[i].f = a != a ? a : a <= 0 ? 0 : a >= 1 ? 1 : a; break;
                case 5: d[i].f = a != a ? a : a <= -1 ? -1 : a >= 1 ? 1 : a; break;
                case 16: d[i].f = 1 / a; break;
                case 17: d[i].f = 1 / sqrtf(a); break;
                case 18: d[i].f = sinq(a); break;
                case 19: d[i].f = cosq(a); break;
                case 20: d[i].f = powf(2, a); break;
                case 21: d[i].f = log2f(a); break;
                case 22: d[i].f = sqrtf(a); break;
                case 23: d[i].f = asinf(a) * 2 / PI; break;
                case 24: d[i].f = -1 / a; break;
                case 26: d[i].f = -sinq(a); break;
                case 28: d[i].f = powf(2, 1 / a); break;
                default: missing(op); d[i].f = 0; break;
                }
            }
            if (sub == 0 || sub == 1 || sub == 2) wr(d, n, vd); else wr(d, n, vd);
        } else if (group == 1) {
            rd(s, n, vs, S_PREFIX);
            V o[4]; memset(o, 0, sizeof o);
            switch (sub) {
            case 24: for (int i = 0; i < 4; i++) { const u32 b = (s[0].w >> (i * 8)) & 255; o[i].w = (b | b << 8 | b << 16 | b << 24) >> 1; } wr_int(o, 4, vd); break;
            case 25: for (int i = 0; i < 4; i++) { const u32 b = (s[0].w >> (i * 8)) & 255; o[i].w = b | b << 8 | b << 16 | b << 24; } wr_int(o, 4, vd); break;
            case 26: case 27: {
                for (int i = 0; i < n; i++) {
                    const u32 lo = s[i].w << 16, hi = s[i].w & 0xFFFF0000u;
                    o[i * 2].w = sub == 26 ? lo >> 1 : lo;
                    o[i * 2 + 1].w = sub == 26 ? hi >> 1 : hi;
                }
                wr_int(o, n * 2, vd); break;
            }
            case 28: for (int i = 0; i < 4; i++) { const s32 v = s[i].i; o[0].w |= (u32)(v < 0 ? 0 : (v >> 23) & 255) << (i * 8); } wr_int(o, 1, vd); break;
            case 29: for (int i = 0; i < 4; i++) o[0].w |= (u32)((s[i].i >> 24) & 255) << (i * 8); wr_int(o, 1, vd); break;
            case 30: case 31: {
                for (int i = 0; i < n; i++) {
                    const s32 v = s[i].i; const u32 h = sub == 31 ? (u32)(v >> 16) & 0xFFFF : (u32)(v < 0 ? 0 : (v >> 15) & 0xFFFF);
                    o[i / 2].w |= h << (i & 1 ? 16 : 0);
                }
                wr_int(o, n / 2, vd); break;
            }
            default: missing(op); break;
            }
        } else if (group == 2) {
            rd(s, n, vs, S_PREFIX);
            float sum = 0;
            switch (sub) {
            case 4: for (int i = 0; i < n; i++) d[i].f = 1 - s[i].f; wr(d, n, vd); break;
            case 5: {                              /* vsocp: x becomes 1-x, x */
                V o[4];
                for (int i = 0; i < n && i < 2; i++) {
                    const float a = s[i].f, b = 1 - a;
                    o[i * 2].f = b < 0 ? 0 : b > 1 ? 1 : b;
                    o[i * 2 + 1].f = a < 0 ? 0 : a > 1 ? 1 : a;
                }
                wr(o, n * 2, vd); break;
            }
            case 6: for (int i = 0; i < n; i++) sum += s[i].f; d[0].f = sum; wr(d, 1, vd); break;
            case 7: for (int i = 0; i < n; i++) sum += s[i].f; d[0].f = sum / n; wr(d, 1, vd); break;
            case 10: for (int i = 0; i < n; i++) d[i].f = s[i].f > 0 ? 1 : s[i].f < 0 ? -1 : 0; wr(d, n, vd); break;
            default: missing(op); break;
            }
        } else if (group == 3) {                   /* vcst */
            static const float k[20] = { 0, 3.402823e38f, 1.4142135f, 0.70710677f, 1.1283792f, 0.63661977f, 0.31830987f, 0.7853982f,
                1.5707964f, 3.1415927f, 2.7182817f, 1.4426950f, 0.43429446f, 0.6931472f, 2.3025851f, 6.2831855f, 0.5235988f, 0.30103f, 3.3219280f, 0.8660254f };
            for (int i = 0; i < n; i++) d[i].f = sub < 20 ? k[sub] : 0;
            wr(d, n, vd);
        } else if (group >= 16 && group <= 19) {
            const double scale = ldexp(1.0, sub);
            rd(s, n, vs, S_PREFIX);
            for (int i = 0; i < n; i++) {
                const double v = (double)s[i].f * scale;
                d[i].i = to_int(group == 16 ? rint(v) : group == 17 ? trunc(v) : group == 18 ? ceil(v) : floor(v));
            }
            wr_int(d, n, vd);
        } else if (group == 20) {
            rd(s, n, vs, S_PREFIX);
            for (int i = 0; i < n; i++) d[i].f = (float)ldexp((double)s[i].i, -sub);
            wr(d, n, vd);
        } else if (group == 21) {                  /* vcmov */
            const int tf = (op >> 19) & 1, imm = (op >> 16) & 7;
            rd(s, n, vs, S_PREFIX); rd_raw(d, n, vd);
            for (int i = 0; i < n; i++) {
                const int bit = imm < 6 ? imm : i;
                if (((p.vcc >> bit) & 1) == (u32)!tf) d[i] = s[i];
            }
            wr_int(d, n, vd);
        } else missing(op);
        prefix_done();
        return;
    }
    if (major == 60) {
        const int ins = (op >> 23) & 7;
        u8 ms[16], mt[16], md[16];
        V a[16], o[16];
        if (ins == 0 || ins == 4 || ins == 7) mtx_regs(md, n, vd);
        if (ins == 0) {                            /* vmmul: the first operand's rows meet the second's columns */
            mtx_regs(ms, n, vs); mtx_regs(mt, n, vt);
            for (int i = 0; i < n * 4; i++) { o[i].f = 0; }
            for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) {
                float sum = 0;
                for (int c = 0; c < n; c++) { V u, v; u.w = vget(ms[y * 4 + c]); v.w = vget(mt[x * 4 + c]); sum += u.f * v.f; }
                o[x * 4 + y].f = sum;
            }
            for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) vset(md[x * 4 + y], o[x * 4 + y].w);
        } else if (ins >= 1 && ins <= 3) {         /* vtfm and vhtfm: n equal to ins means a 1 is implied last */
            const int side = ins + 1, home = n == ins;
            mtx_regs(ms, side, vs);
            rd_raw(t, n, vt);
            for (int i = 0; i < side; i++) {
                float sum = 0;
                for (int k = 0; k < n; k++) { V u; u.w = vget(ms[i * 4 + k]); sum += u.f * t[k].f; }
                if (home) { V u; u.w = vget(ms[i * 4 + ins]); sum += u.f; }
                d[i].f = sum;
            }
            wr(d, side, vd);
        } else if (ins == 4) {                     /* vmscl */
            mtx_regs(ms, n, vs);
            rd_raw(t, 1, vt);
            for (int i = 0; i < n * 4; i += 1) if (i / 4 < n && i % 4 < n) { V u; u.w = vget(ms[i]); u.f *= t[0].f; vset(md[i], u.w); }
        } else if (ins == 5) {
            rd(s, n, vs, S_PREFIX); rd(t, n, vt, T_PREFIX);
            if (n == 3) {
                d[0].f = s[1].f * t[2].f - s[2].f * t[1].f;
                d[1].f = s[2].f * t[0].f - s[0].f * t[2].f;
                d[2].f = s[0].f * t[1].f - s[1].f * t[0].f;
            } else if (n == 4) {
                d[0].f = s[0].f * t[3].f + s[1].f * t[2].f - s[2].f * t[1].f + s[3].f * t[0].f;
                d[1].f = -s[0].f * t[2].f + s[1].f * t[3].f + s[2].f * t[0].f + s[3].f * t[1].f;
                d[2].f = s[0].f * t[1].f - s[1].f * t[0].f + s[2].f * t[3].f + s[3].f * t[2].f;
                d[3].f = -s[0].f * t[0].f - s[1].f * t[1].f - s[2].f * t[2].f + s[3].f * t[3].f;
            } else missing(op);
            if (n >= 3) wr(d, n, vd);
        } else if (ins == 7) {
            if (op & 0x200000) {                   /* vrot */
                const int imm = (op >> 16) & 31, sl = (imm >> 2) & 3, cl = imm & 3;
                rd_raw(s, 1, vs);
                float si = sinq(s[0].f); const float co = cosq(s[0].f);
                if (imm & 16) si = -si;
                for (int i = 0; i < n; i++) d[i].f = sl == cl || i == sl ? si : 0;
                d[cl].f = co;
                wr(d, n, vd);
            } else {
                const int sub = (op >> 16) & 15;
                if (sub == 0) {                    /* vmmov */
                    mtx_regs(ms, n, vs);
                    for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) a[x * 4 + y].w = vget(ms[x * 4 + y]);
                    for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) vset(md[x * 4 + y], a[x * 4 + y].w);
                } else if (sub == 3 || sub == 6 || sub == 7) {
                    for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) {
                        V u; u.f = sub == 3 ? x == y : sub == 7;
                        vset(md[x * 4 + y], u.w);
                    }
                } else missing(op);
            }
        } else missing(op);
        prefix_done();
        return;
    }
    missing(op);
}

static int vreg(int m, int col, int row) { return m << 2 | col | row << 5; }
void psp_vfpu_load(u32 inst) {
    const u32 op = inst >> 26, a = p.r[(inst >> 21) & 31] + (u32)(s32)(s16)(inst & 0xFFFC);
    if (op == 50 || op == 58) {
        const int r = ((inst >> 16) & 31) | (inst & 3) << 5;
        if (op == 50) vset(r, RD32(a)); else WR32(a, vget(r));
        return;
    }
    const int vt = ((inst >> 16) & 31) | (inst & 1) << 5, m = (vt >> 2) & 7, i = vt & 3, row = (vt >> 5) & 1;
    int r[4];
    for (int k = 0; k < 4; k++) r[k] = row ? vreg(m, k, i) : vreg(m, i, k);
    if (op == 54) for (int k = 0; k < 4; k++) vset(r[k], RD32(a + k * 4));
    else if (op == 62) for (int k = 0; k < 4; k++) WR32(a + k * 4, vget(r[k]));
    else {                                         /* unaligned quads, left and right halves */
        const u32 off = (a >> 2) & 3, base = a & ~15u;
        if (op == 53) {
            if (inst & 2) { for (u32 k = off; k < 4; k++) vset(r[k - off], RD32(base + k * 4)); }
            else for (u32 k = 0; k <= off; k++) vset(r[3 - k], RD32(base + (off - k) * 4));
        } else {
            if (inst & 2) { for (u32 k = off; k < 4; k++) WR32(base + k * 4, vget(r[k - off])); }
            else for (u32 k = 0; k <= off; k++) WR32(base + (off - k) * 4, vget(r[3 - k]));
        }
    }
}

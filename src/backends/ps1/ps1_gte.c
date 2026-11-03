/*
 * ps1_gte.c - the Geometry Transformation Engine, coprocessor 2: fixed-point
 * matrices, perspective with the hardware's own divide, lighting and depth cue,
 * with every saturation flag the games can see.
 */
#include "ps1.h"

static s16 V[3][3];                 /* three vectors */
static u8 RGBC[4];
static u16 OTZ;
static s16 IR[4];
static s32 SXY[3][2];
static u16 SZ[4];
static u8 RGB[3][4];
static u32 RES1;
static s32 MAC[4];
static u32 LZCS, LZCR;
static s16 M[3][3][3];              /* rotation, light, color */
static s32 TR[3], BK[3], FC[3];
static s32 OFX, OFY;
static u16 H;
static s16 DQA, ZSF3, ZSF4;
static s32 DQB;
static u32 FLAG;
static u8 unr[257];
static int sf, lm;

static inline s64 ext44(int i, s64 v) {
    if (v > 0x7ffffffffffLL) FLAG |= 1u << (31 - i);
    else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);
    return (s64)((u64)v << 20) >> 20;
}

static inline s32 sat_ir(int i, s64 v, int low0) {
    const s32 lo = low0 ? 0 : -0x8000;
    if (v < lo) { FLAG |= 1u << (25 - i); return lo; }
    if (v > 0x7fff) { FLAG |= 1u << (25 - i); return 0x7fff; }
    return (s32)v;
}

static inline void set_mac(int i, s64 v) { MAC[i] = (s32)(ext44(i, v) >> (sf * 12)); }
static inline void set_ir(int i, int low0) { IR[i] = sat_ir(i, MAC[i], low0); }

static inline s64 mac0(s64 v) {
    if (v > 0x7fffffffLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
    return v;
}

static inline u8 sat_color(int i, s32 v) {
    if (v < 0) { FLAG |= 1u << (21 - i); return 0; }
    if (v > 255) { FLAG |= 1u << (21 - i); return 255; }
    return v;
}

static void push_color(void) {
    memcpy(RGB[0], RGB[1], 4);
    memcpy(RGB[1], RGB[2], 4);
    RGB[2][0] = sat_color(0, MAC[1] >> 4);
    RGB[2][1] = sat_color(1, MAC[2] >> 4);
    RGB[2][2] = sat_color(2, MAC[3] >> 4);
    RGB[2][3] = RGBC[3];
}

static void push_z(s64 z) {
    SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
    if (z < 0) { FLAG |= 1u << 18; z = 0; }
    else if (z > 0xffff) { FLAG |= 1u << 18; z = 0xffff; }
    SZ[3] = (u16)z;
}

static u32 divide(void) {
    if (H >= SZ[3] * 2) { FLAG |= 1u << 17; return 0x1ffff; }
    const int z = __builtin_clz(SZ[3]) - 16;
    const u32 n = (u32)H << z;
    u32 d = (u32)SZ[3] << z;
    const u32 u = unr[(d - 0x7fc0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    const u64 q = ((u64)n * d + 0x8000) >> 16;
    return q > 0x1ffff ? 0x1ffff : (u32)q;
}

/* a vector times a matrix, plus a translation shifted up 12 bits */
static void transform(const s16 m[3][3], const s16 *v, const s32 *t) {
    for (int i = 0; i < 3; i++) {
        s64 a = t ? (s64)t[i] << 12 : 0;
        a = ext44(i + 1, a + (s64)m[i][0] * v[0]);
        a = ext44(i + 1, a + (s64)m[i][1] * v[1]);
        a = ext44(i + 1, a + (s64)m[i][2] * v[2]);
        MAC[i + 1] = (s32)(a >> (sf * 12));
    }
    for (int i = 1; i <= 3; i++) set_ir(i, lm);
}

static void rtp(int n, int last) {
    s64 a3 = 0;
    for (int i = 0; i < 3; i++) {
        s64 a = (s64)TR[i] << 12;
        a = ext44(i + 1, a + (s64)M[0][i][0] * V[n][0]);
        a = ext44(i + 1, a + (s64)M[0][i][1] * V[n][1]);
        a = ext44(i + 1, a + (s64)M[0][i][2] * V[n][2]);
        MAC[i + 1] = (s32)(a >> (sf * 12));
        if (i == 2) a3 = a;
    }
    set_ir(1, lm);
    set_ir(2, lm);
    {   /* IR3's flag follows MAC3 >> 12 whatever sf says */
        const s64 z = a3 >> 12;
        if (z < -0x8000 || z > 0x7fff) FLAG |= 1u << 22;
        const s32 lo = lm ? 0 : -0x8000;
        IR[3] = MAC[3] < lo ? lo : MAC[3] > 0x7fff ? 0x7fff : MAC[3];
    }
    push_z(a3 >> 12);
    const s64 q = divide();
    s64 x = mac0(q * IR[1] + OFX), y = mac0(q * IR[2] + OFY);
    MAC[0] = (s32)y;
    x >>= 16; y >>= 16;
    if (x < -0x400) { FLAG |= 1u << 14; x = -0x400; } else if (x > 0x3ff) { FLAG |= 1u << 14; x = 0x3ff; }
    if (y < -0x400) { FLAG |= 1u << 13; y = -0x400; } else if (y > 0x3ff) { FLAG |= 1u << 13; y = 0x3ff; }
    memcpy(SXY[0], SXY[1], sizeof SXY[0]);
    memcpy(SXY[1], SXY[2], sizeof SXY[0]);
    SXY[2][0] = (s32)x; SXY[2][1] = (s32)y;
    if (last) {
        const s64 d = mac0(q * DQA + DQB);
        MAC[0] = (s32)d;
        s64 i0 = d >> 12;
        if (i0 < 0) { FLAG |= 1u << 12; i0 = 0; } else if (i0 > 0x1000) { FLAG |= 1u << 12; i0 = 0x1000; }
        IR[0] = (s16)i0;
    }
}

/* toward the far color by IR0, then into the color FIFO */
static void depth_cue(const s64 *in) {
    for (int i = 0; i < 3; i++) {
        const s32 d = sat_ir(i + 1, ext44(i + 1, ((s64)FC[i] << 12) - in[i]) >> (sf * 12), 0);
        MAC[i + 1] = (s32)(ext44(i + 1, (s64)d * IR[0] + in[i]) >> (sf * 12));
    }
    push_color();
    for (int i = 1; i <= 3; i++) set_ir(i, lm);
}

static void light(int n) {         /* normal through the light then the color matrix */
    transform(M[1], V[n], NULL);
    const s16 ir[3] = { IR[1], IR[2], IR[3] };
    transform(M[2], ir, BK);
}

static void color_stage(int cue, const u8 *c) {
    s64 m[3];
    for (int i = 0; i < 3; i++) m[i] = ((s64)c[i] * IR[i + 1]) << 4;
    if (cue) { depth_cue(m); return; }
    for (int i = 0; i < 3; i++) set_mac(i + 1, m[i]);
    push_color();
    for (int i = 1; i <= 3; i++) set_ir(i, lm);
}

static void nc(int n, int color, int cue) {
    light(n);
    if (color) color_stage(cue, RGBC);
    else {
        push_color();
    }
}

static void mvmva(u32 op) {
    const int mx = (op >> 17) & 3, vx = (op >> 15) & 3, cv = (op >> 13) & 3;
    s16 v[3];
    if (vx == 3) { v[0] = IR[1]; v[1] = IR[2]; v[2] = IR[3]; }
    else memcpy(v, V[vx], sizeof v);
    s16 m[3][3];
    if (mx == 3) {
        const s16 g[3][3] = { { (s16)-(RGBC[0] << 4), (s16)(RGBC[0] << 4), IR[0] },
                              { M[0][0][2], M[0][0][2], M[0][0][2] }, { M[0][1][1], M[0][1][1], M[0][1][1] } };
        memcpy(m, g, sizeof m);
    } else memcpy(m, M[mx], sizeof m);
    const s32 zero[3] = { 0, 0, 0 };
    const s32 *t = cv == 0 ? TR : cv == 1 ? BK : cv == 2 ? FC : zero;
    if (cv == 2) {      /* the far color column is broken: it only sets flags */
        for (int i = 0; i < 3; i++) {
            const s64 a = ext44(i + 1, ((s64)t[i] << 12) + (s64)m[i][0] * v[0]);
            sat_ir(i + 1, a >> (sf * 12), 0);
            s64 b = ext44(i + 1, (s64)m[i][1] * v[1]);
            b = ext44(i + 1, b + (s64)m[i][2] * v[2]);
            MAC[i + 1] = (s32)(b >> (sf * 12));
        }
        for (int i = 1; i <= 3; i++) set_ir(i, lm);
        return;
    }
    transform((const s16 (*)[3])m, v, t);
}

void gte_command(u32 op) {
    static int ready;
    if (!ready) {
        for (int i = 0; i < 257; i++) { const int u = (0x40000 / (i + 0x100) + 1) / 2 - 0x101; unr[i] = u > 0 ? u : 0; }
        ready = 1;
    }
    sf = (op >> 19) & 1;
    lm = (op >> 10) & 1;
    FLAG = 0;
    switch (op & 63) {
    case 0x01: rtp(0, 1); break;
    case 0x06: MAC[0] = (s32)mac0((s64)SXY[0][0] * SXY[1][1] + (s64)SXY[1][0] * SXY[2][1] + (s64)SXY[2][0] * SXY[0][1]
                                - (s64)SXY[0][0] * SXY[2][1] - (s64)SXY[1][0] * SXY[0][1] - (s64)SXY[2][0] * SXY[1][1]); break;
    case 0x0c: {
        const s32 d1 = M[0][0][0], d2 = M[0][1][1], d3 = M[0][2][2];
        set_mac(1, (s64)IR[3] * d2 - (s64)IR[2] * d3);
        set_mac(2, (s64)IR[1] * d3 - (s64)IR[3] * d1);
        set_mac(3, (s64)IR[2] * d1 - (s64)IR[1] * d2);
        for (int i = 1; i <= 3; i++) set_ir(i, lm);
        break;
    }
    case 0x10: { s64 m[3]; for (int i = 0; i < 3; i++) m[i] = (s64)RGBC[i] << 16; depth_cue(m); break; }
    case 0x11: { s64 m[3]; for (int i = 0; i < 3; i++) m[i] = (s64)IR[i + 1] << 12; depth_cue(m); break; }
    case 0x12: mvmva(op); break;
    case 0x13: nc(0, 1, 1); break;
    case 0x14: { transform(M[2], (s16[]){ IR[1], IR[2], IR[3] }, BK); color_stage(1, RGBC); break; }
    case 0x16: for (int n = 0; n < 3; n++) nc(n, 1, 1); break;
    case 0x1b: nc(0, 1, 0); break;
    case 0x1c: { transform(M[2], (s16[]){ IR[1], IR[2], IR[3] }, BK); color_stage(0, RGBC); break; }
    case 0x1e: nc(0, 0, 0); break;
    case 0x20: for (int n = 0; n < 3; n++) nc(n, 0, 0); break;
    case 0x28: for (int i = 1; i <= 3; i++) { set_mac(i, (s64)IR[i] * IR[i]); set_ir(i, lm); } break;
    case 0x29: color_stage(1, RGBC); break;
    case 0x2a: for (int k = 0; k < 3; k++) { s64 m[3]; for (int i = 0; i < 3; i++) m[i] = (s64)RGB[0][i] << 16; depth_cue(m); } break;
    case 0x2d: { const s64 v = mac0((s64)ZSF3 * (SZ[1] + SZ[2] + SZ[3])); MAC[0] = (s32)v; s64 z = v >> 12;
                 if (z < 0) { FLAG |= 1u << 18; z = 0; } else if (z > 0xffff) { FLAG |= 1u << 18; z = 0xffff; } OTZ = (u16)z; break; }
    case 0x2e: { const s64 v = mac0((s64)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3])); MAC[0] = (s32)v; s64 z = v >> 12;
                 if (z < 0) { FLAG |= 1u << 18; z = 0; } else if (z > 0xffff) { FLAG |= 1u << 18; z = 0xffff; } OTZ = (u16)z; break; }
    case 0x30: rtp(0, 0); rtp(1, 0); rtp(2, 1); break;
    case 0x3d: for (int i = 1; i <= 3; i++) { set_mac(i, (s64)IR[0] * IR[i]); } push_color(); for (int i = 1; i <= 3; i++) set_ir(i, lm); break;
    case 0x3e: for (int i = 1; i <= 3; i++) { set_mac(i, ((s64)MAC[i] << (sf * 12)) + (s64)IR[0] * IR[i]); } push_color(); for (int i = 1; i <= 3; i++) set_ir(i, lm); break;
    case 0x3f: for (int n = 0; n < 3; n++) nc(n, 1, 0); break;
    default: break;
    }
    if (FLAG & 0x7f87e000) FLAG |= 1u << 31;
}

static inline u32 pack16(s16 a, s16 b) { return (u16)a | (u32)(u16)b << 16; }

u32 gte_read(int r) {
    switch (r) {
    case 0: case 2: case 4: return pack16(V[r / 2][0], V[r / 2][1]);
    case 1: case 3: case 5: return (u32)(s32)V[r / 2][2];
    case 6: return RGBC[0] | RGBC[1] << 8 | RGBC[2] << 16 | (u32)RGBC[3] << 24;
    case 7: return OTZ;
    case 8: case 9: case 10: case 11: return (u32)(s32)IR[r - 8];
    case 12: case 13: case 14: return (u16)SXY[r - 12][0] | (u32)(u16)SXY[r - 12][1] << 16;
    case 15: return (u16)SXY[2][0] | (u32)(u16)SXY[2][1] << 16;
    case 16: case 17: case 18: case 19: return SZ[r - 16];
    case 20: case 21: case 22: return RGB[r - 20][0] | RGB[r - 20][1] << 8 | RGB[r - 20][2] << 16 | (u32)RGB[r - 20][3] << 24;
    case 23: return RES1;
    case 24: case 25: case 26: case 27: return (u32)MAC[r - 24];
    case 28: case 29: {
        u32 c = 0;
        for (int i = 0; i < 3; i++) { s32 v = IR[i + 1] >> 7; v = v < 0 ? 0 : v > 31 ? 31 : v; c |= (u32)v << (5 * i); }
        return c;
    }
    case 30: return LZCS;
    case 31: return LZCR;
    case 32 + 0: case 32 + 1: case 32 + 2: case 32 + 3: case 32 + 8: case 32 + 9: case 32 + 10: case 32 + 11:
    case 32 + 16: case 32 + 17: case 32 + 18: case 32 + 19: {
        const int b = (r - 32) / 8, k = (r - 32) % 8 * 2;
        const s16 *m = &M[b][0][0];
        return pack16(m[k], m[k + 1]);
    }
    case 32 + 4: return (u32)(s32)M[0][2][2];
    case 32 + 12: return (u32)(s32)M[1][2][2];
    case 32 + 20: return (u32)(s32)M[2][2][2];
    case 32 + 5: case 32 + 6: case 32 + 7: return (u32)TR[r - 37];
    case 32 + 13: case 32 + 14: case 32 + 15: return (u32)BK[r - 45];
    case 32 + 21: case 32 + 22: case 32 + 23: return (u32)FC[r - 53];
    case 32 + 24: return (u32)OFX;
    case 32 + 25: return (u32)OFY;
    case 32 + 26: return (u32)(s32)(s16)H;      /* read back sign-extended, a hardware bug */
    case 32 + 27: return (u32)(s32)DQA;
    case 32 + 28: return (u32)DQB;
    case 32 + 29: return (u32)(s32)ZSF3;
    case 32 + 30: return (u32)(s32)ZSF4;
    case 32 + 31: return FLAG;
    }
    return 0;
}

void gte_write(int r, u32 v) {
    switch (r) {
    case 0: case 2: case 4: V[r / 2][0] = (s16)v; V[r / 2][1] = (s16)(v >> 16); break;
    case 1: case 3: case 5: V[r / 2][2] = (s16)v; break;
    case 6: RGBC[0] = v; RGBC[1] = v >> 8; RGBC[2] = v >> 16; RGBC[3] = v >> 24; break;
    case 7: OTZ = (u16)v; break;
    case 8: case 9: case 10: case 11: IR[r - 8] = (s16)v; break;
    case 12: case 13: case 14: SXY[r - 12][0] = (s16)v; SXY[r - 12][1] = (s16)(v >> 16); break;
    case 15:
        memcpy(SXY[0], SXY[1], sizeof SXY[0]);
        memcpy(SXY[1], SXY[2], sizeof SXY[0]);
        SXY[2][0] = (s16)v; SXY[2][1] = (s16)(v >> 16);
        break;
    case 16: case 17: case 18: case 19: SZ[r - 16] = (u16)v; break;
    case 20: case 21: case 22: RGB[r - 20][0] = v; RGB[r - 20][1] = v >> 8; RGB[r - 20][2] = v >> 16; RGB[r - 20][3] = v >> 24; break;
    case 23: RES1 = v; break;
    case 24: case 25: case 26: case 27: MAC[r - 24] = (s32)v; break;
    case 28:
        IR[1] = (v & 0x1f) << 7; IR[2] = ((v >> 5) & 0x1f) << 7; IR[3] = ((v >> 10) & 0x1f) << 7;
        break;
    case 29: break;
    case 30: LZCS = v; LZCR = (s32)v < 0 ? (~v ? __builtin_clz(~v) : 32) : (v ? __builtin_clz(v) : 32); break;
    case 31: break;
    case 32 + 0: case 32 + 1: case 32 + 2: case 32 + 3: case 32 + 8: case 32 + 9: case 32 + 10: case 32 + 11:
    case 32 + 16: case 32 + 17: case 32 + 18: case 32 + 19: {
        const int b = (r - 32) / 8, k = (r - 32) % 8 * 2;
        s16 *m = &M[b][0][0];
        m[k] = (s16)v; m[k + 1] = (s16)(v >> 16);
        break;
    }
    case 32 + 4: M[0][2][2] = (s16)v; break;
    case 32 + 12: M[1][2][2] = (s16)v; break;
    case 32 + 20: M[2][2][2] = (s16)v; break;
    case 32 + 5: case 32 + 6: case 32 + 7: TR[r - 37] = (s32)v; break;
    case 32 + 13: case 32 + 14: case 32 + 15: BK[r - 45] = (s32)v; break;
    case 32 + 21: case 32 + 22: case 32 + 23: FC[r - 53] = (s32)v; break;
    case 32 + 24: OFX = (s32)v; break;
    case 32 + 25: OFY = (s32)v; break;
    case 32 + 26: H = (u16)v; break;
    case 32 + 27: DQA = (s16)v; break;
    case 32 + 28: DQB = (s32)v; break;
    case 32 + 29: ZSF3 = (s16)v; break;
    case 32 + 30: ZSF4 = (s16)v; break;
    case 32 + 31: FLAG = v & 0x7ffff000; if (FLAG & 0x7f87e000) FLAG |= 1u << 31; break;
    }
}

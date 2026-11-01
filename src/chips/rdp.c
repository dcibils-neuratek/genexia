/*
 * rdp.c - Nintendo 64 Reality Display Processor (software rasterizer).
 *
 * Commands arrive through rdp_run() and are executed synchronously, writing
 * straight into n64_rdram. The model follows the real pipeline: edge walker
 * (4 sub-scanlines, 8 coverage samples per pixel), texture coordinate unit,
 * TMEM, colour/alpha combiner, Z unit and blender. Spans are drawn left to
 * right through per-mode specialised loops; the attribute planes are evaluated
 * at each pixel's top-left sample, as the hardware does.
 *
 * Everything that is emulated state lives in S (tagged N64_STATE) plus the
 * hidden RDRAM bits. The derived tables (M, P and the combiner inputs) hold
 * pointers, so they are not state: prep() rebuilds them from S whenever the
 * modes differ, which also makes loading a save state safe.
 */
#include "n64.h"
#include <stdio.h>
#include <stdlib.h>
int rdp_debug;
#include <string.h>

#define ALWAYS   static inline __attribute__((always_inline))
#define AMASK    ((u32)(N64_RDRAM_SIZE - 1))
#define HI(x)    ((u32)((x) >> 32))
#define LO(x)    ((u32)(x))

typedef s32 v4  __attribute__((vector_size(16)));
typedef u8  v4b __attribute__((vector_size(4)));
typedef union { v4 v; s32 c[4]; } Col;

ALWAYS v4 v4s(s32 x) { return (v4){x, x, x, x}; }
ALWAYS v4 vclamp255(v4 x) {
    return __builtin_elementwise_min(__builtin_elementwise_max(x, v4s(0)), v4s(255));
}
/* 9-bit combiner value to its signed range: 0x180..0x1FF are negative. */
ALWAYS v4 vext9(v4 x) { return ((x + 128) & 0x1FF) - 128; }
ALWAYS s32 ext9(s32 x) { return ((x + 128) & 0x1FF) - 128; }
ALWAYS s32 sx(u32 v, int bits) { return (s32)(v << (32 - bits)) >> (32 - bits); }
ALWAYS u16 be16(const u8 *p) { u16 v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
ALWAYS void wbe16(u8 *p, u16 v) { v = __builtin_bswap16(v); memcpy(p, &v, 2); }

/* ------------------------------------------------------------------ state */
typedef struct {
    u8  fmt, size, pal, ct, mt, cs, ms, mask_t, mask_s, shift_t, shift_s;
    u8  clamp_s, clamp_t, kind, m_s, m_t;  /* derived, plain data */
    u16 line, tmem, sl, tl, sh, th, cds, cdt, wrap_s, wrap_t;
} Tile;

typedef struct {
    u8   tmem[4096];
    Tile tile[8];
    u64  cmd[22];
    int  cmd_fill;
    u64  om, combine;
    u32  fill, prim, env, blend, fog;
    int  prim_lod_min, prim_lod_frac;
    int  key_w[3], key_c[3], key_s[3];
    int  k[6];
    int  sc_xh, sc_yh, sc_xl, sc_yl, sc_field, sc_odd;    /* 10.2 */
    u32  ti_addr, fb_addr, z_addr;
    int  ti_fmt, ti_size, ti_width, fb_fmt, fb_size, fb_width;
    int  prim_z, prim_dz;
    u32  rng;
} RdpState;

static RdpState S N64_STATE;
/* The two extra bits per 16-bit RDRAM word (coverage or low dz bits). */
static u8 hidden[N64_RDRAM_SIZE / 2] N64_STATE;

/* Enhancement: high-resolution rendering. Every draw runs twice. The first run
 * is the real one, into RDRAM, so the game sees exactly what the console would
 * produce. The second is the same draw scaled up by hr_n into a host buffer
 * that is not emulated state; S is put back afterwards, so nothing the game can
 * observe changes. These pointers select which memory the pixel code touches. */
static u8 *R_ram = n64_rdram, *R_hid = hidden;
static u32 R_mask = AMASK;
static int hr_n = 1;               /* scale of the draw running now */

/* ---------------------------------------------------- derived pipeline state */
typedef void (*SampleFn)(const Tile *, s32, s32, Col *);

typedef struct { s16 ql[4], qr[4]; int y, x0, x1, full0, full1; v4 rgba; u32 s, t, w, z; } Span;
typedef void (*SpanFn)(const Span *);

typedef struct { const Col *a, *b, *c, *d; const s32 *aa, *ab, *ac, *ad; } CC;

static struct {
    int valid;
    u64 om, comb;
    int cycle, two, persp, lod_en, detail, sharpen, tlut, tlut_ia, bil, mid, key;
    int rgb_dith, alpha_dith, force_blend, alpha_cvg, cvg_x_alpha, z_mode, cvg_dest;
    int color_on_cvg, image_read, z_update, z_compare, aa, z_sel, dither_alpha, acmp;
    int need0, need1, need_n, use_lod, use_noise, use_shade, use_shade_bl;
    int spl_t0, spl_t1, spl_sh, spl_cm, spl_ca;
    int mem_always, partial, special;
    int special_c[2];
    CC  cc[2];
    const Col *bP[2], *bM[2];
    const s32 *bA[2], *bB[2];
    u8  dr[16], da[16];
} M;

static struct {
    int tile, level, dzpix, dzenc, zsel; u32 zprim; int acthresh;
    v4 drgba; u32 ds, dt, dw, dz, dsy, dty, dwy;
    const Tile *tl0, *tl1; SampleFn fn0, fn1;
    int texm;
} P;

/* combiner inputs, kept out of the save state (see top comment) */
static Col c_combined, c_combm, c_tex0, c_tex1, c_texn, c_prim, c_shade, c_env, c_one,
           c_noise, c_zero, c_keyc, c_keys, c_k4, c_k5, s_comb, s_tex0, s_tex1, s_texn,
           s_prim, s_shade, s_env, s_lod, s_plod;
/* blender inputs */
static Col c_pixel, c_blended, c_mem, c_blend, c_fog;
static s32 px_shade_a, px_inv;
static const s32 k_255 = 255, k_0 = 0;
static int px_blsa, px_blsb;

/* ----------------------------------------------------------- Z number formats */
static const struct { int shift; int add; } z_dec[8] = {
    {6, 0x00000}, {5, 0x20000}, {4, 0x30000}, {3, 0x38000},
    {2, 0x3C000}, {1, 0x3E000}, {0, 0x3F000}, {0, 0x3F800},
};
ALWAYS u32 z_decompress(u32 v14) {
    int e = (v14 >> 11) & 7;
    return ((v14 & 0x7FF) << z_dec[e].shift) + (u32)z_dec[e].add;
}
ALWAYS u32 z_compress(u32 z18) {
    int n = __builtin_clz(~(z18 << 14));   /* leading ones of the 18-bit value */
    if (n > 7) n = 7;
    return ((u32)n << 11) | ((z18 >> z_dec[n].shift) & 0x7FF);
}
static int dz_normalize(int sum) {
    if (sum & 0xC000) return 0x8000;
    if (!(sum & 0xFFFF)) return 1;
    for (int b = 0x2000; b > 0; b >>= 1) if (sum & b) return b << 1;
    return 1;
}
static int dz_encode(int dz) { return dz > 0 ? 31 - __builtin_clz((u32)dz & 0xFFFF) : 0; }

/* -------------------------------------------------------------- texel fetch */
enum { K_RGBA16, K_RGBA32, K_I4, K_I8, K_IA4, K_IA8, K_IA16, K_CI4, K_P4R, K_P8R, K_P16R,
       K_P4I, K_P8I, K_P16I, K_COUNT };

ALWAYS u32 rgba5551(u32 c) {
    u32 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return r | g << 8 | b << 16 | (c & 1 ? 0xFF000000u : 0);
}
ALWAYS u32 grey(u32 i, u32 a) { return i | i << 8 | i << 16 | a << 24; }
ALWAYS u32 pal_r(u32 idx) { return rgba5551(be16(&S.tmem[0x800 + idx * 8])); }
ALWAYS u32 pal_i(u32 idx) { u32 c = be16(&S.tmem[0x800 + idx * 8]); return grey(c >> 8, c & 0xFF); }

ALWAYS u32 fetch(const int kind, const Tile *tl, s32 s, s32 t) {
    u32 b = ((u32)tl->tmem + (u32)tl->line * (u32)t) << 3, c;
    switch (kind) {
    case K_RGBA16:
        return rgba5551(be16(&S.tmem[(b + (u32)s * 2) & 0xFFE]));
    case K_RGBA32: {
        u32 a = (b + (u32)s * 2) & 0x7FE;
        return (u32)S.tmem[a] | (u32)S.tmem[a + 1] << 8 |
               (u32)S.tmem[a | 0x800] << 16 | (u32)S.tmem[(a | 0x800) + 1] << 24;
    }
    case K_I4:
        c = S.tmem[(b + ((u32)s >> 1)) & 0xFFF];
        c = (s & 1) ? c & 15 : c >> 4;
        c |= c << 4;
        return grey(c, c);
    case K_I8:
        c = S.tmem[(b + (u32)s) & 0xFFF];
        return grey(c, c);
    case K_IA4: {
        c = S.tmem[(b + ((u32)s >> 1)) & 0xFFF];
        c = (s & 1) ? c & 15 : c >> 4;
        u32 i = c & 14;
        return grey((i << 4) | (i << 1) | (i >> 2), c & 1 ? 255 : 0);
    }
    case K_IA8: {
        c = S.tmem[(b + (u32)s) & 0xFFF];
        u32 i = c & 0xF0, a = c & 15;
        return grey(i | (i >> 4), a | (a << 4));
    }
    case K_IA16:
        c = be16(&S.tmem[(b + (u32)s * 2) & 0xFFE]);
        return grey(c >> 8, c & 0xFF);
    case K_CI4:
        c = S.tmem[(b + ((u32)s >> 1)) & 0xFFF];
        c = (s & 1) ? c & 15 : c >> 4;
        c |= (u32)tl->pal << 4;
        return grey(c, c);
    case K_P4R: case K_P4I:
        c = S.tmem[(b + ((u32)s >> 1)) & 0x7FF];
        c = (s & 1) ? c & 15 : c >> 4;
        c |= (u32)tl->pal << 4;
        return kind == K_P4R ? pal_r(c) : pal_i(c);
    case K_P8R:  return pal_r(S.tmem[(b + (u32)s) & 0x7FF]);
    case K_P8I:  return pal_i(S.tmem[(b + (u32)s) & 0x7FF]);
    case K_P16R: return pal_r(S.tmem[(b + (u32)s * 2) & 0x7FE]);
    default:     return pal_i(S.tmem[(b + (u32)s * 2) & 0x7FE]);
    }
}

ALWAYS v4 widen(u32 c) { v4b b; memcpy(&b, &c, 4); return __builtin_convertvector(b, v4); }

ALWAYS s32 tcshift(s32 c, int sh) {
    c = (s16)c;
    return sh < 11 ? c >> sh : (s32)(s16)(c << (16 - sh));
}
ALWAYS s32 tcmask(s32 c, int m, int wrap, int mirror) {
    if (m) {
        if (mirror && ((c >> m) & 1)) c = ~c;
        c &= wrap;
    }
    return c;
}

ALWAYS void sample_tmpl(const int kind, const int bil, const Tile *tl, s32 ss, s32 st, Col *out) {
    s32 s = tcshift(ss, tl->shift_s), t = tcshift(st, tl->shift_t);
    int maxs = (s >> 3) >= (s32)tl->sh, maxt = (t >> 3) >= (s32)tl->th;
    s -= (s32)tl->sl << 3;
    t -= (s32)tl->tl << 3;
    int sf = s & 31, tf = t & 31;
    s >>= 5; t >>= 5;
    if (tl->clamp_s) {
        if (s < 0) { s = 0; sf = 0; }
        else if (maxs) { s = tl->cds; sf = 0; }
    }
    if (tl->clamp_t) {
        if (t < 0) { t = 0; tf = 0; }
        else if (maxt) { t = tl->cdt; tf = 0; }
    }
    s32 s0 = tcmask(s, tl->m_s, tl->wrap_s, tl->ms), t0 = tcmask(t, tl->m_t, tl->wrap_t, tl->mt);
    if (!bil || (sf | tf) == 0) {
        out->v = widen(fetch(kind, tl, s0, t0));
        return;
    }
    s32 s1 = tcmask(s + 1, tl->m_s, tl->wrap_s, tl->ms), t1 = tcmask(t + 1, tl->m_t, tl->wrap_t, tl->mt);
    v4 a = widen(fetch(kind, tl, s0 , t0)), d = widen(fetch(kind, tl, s1, t1));
    v4 b = widen(fetch(kind, tl, s1, t0)), c = widen(fetch(kind, tl, s0, t1));
    if (M.mid && sf == 16 && tf == 16) {
        out->v = (a + b + c + d + 2) >> 2;
    } else if (sf + tf >= 32) {
        out->v = d + (((32 - sf) * (c - d) + (32 - tf) * (b - d) + 16) >> 5);
    } else {
        out->v = a + ((sf * (b - a) + tf * (c - a) + 16) >> 5);
    }
}

#define SMP(k) \
    static void smp0_##k(const Tile *t, s32 s, s32 u, Col *o) { sample_tmpl(k, 0, t, s, u, o); } \
    static void smp1_##k(const Tile *t, s32 s, s32 u, Col *o) { sample_tmpl(k, 1, t, s, u, o); }
SMP(K_RGBA16) SMP(K_RGBA32) SMP(K_I4) SMP(K_I8) SMP(K_IA4) SMP(K_IA8) SMP(K_IA16) SMP(K_CI4)
SMP(K_P4R) SMP(K_P8R) SMP(K_P16R) SMP(K_P4I) SMP(K_P8I) SMP(K_P16I)
#define SMPE(k) { smp0_##k, smp1_##k }
static const SampleFn sample_fn[K_COUNT][2] = {
    SMPE(K_RGBA16), SMPE(K_RGBA32), SMPE(K_I4), SMPE(K_I8), SMPE(K_IA4), SMPE(K_IA8), SMPE(K_IA16),
    SMPE(K_CI4), SMPE(K_P4R), SMPE(K_P8R), SMPE(K_P16R), SMPE(K_P4I), SMPE(K_P8I), SMPE(K_P16I),
};

static int tile_kind(const Tile *t) {
    if (M.tlut && t->size < 3) {
        static const u8 r[3] = {K_P4R, K_P8R, K_P16R}, i[3] = {K_P4I, K_P8I, K_P16I};
        return M.tlut_ia ? i[t->size] : r[t->size];
    }
    if (t->size == 3) return K_RGBA32;
    switch (t->fmt) {
    case 0: case 1: return t->size == 2 ? K_RGBA16 : t->size == 1 ? K_I8 : K_I4;
    case 2:         return t->size == 2 ? K_IA16 : t->size == 1 ? K_I8 : K_CI4;
    case 3:         return t->size == 2 ? K_IA16 : t->size == 1 ? K_IA8 : K_IA4;
    default:        return t->size == 2 ? K_IA16 : t->size == 1 ? K_I8 : K_I4;
    }
}

/* ------------------------------------------------------ perspective and LOD */
ALWAYS void tcdiv(const int persp, s32 ss, s32 st, s32 sw, s32 *os, s32 *ot) {
    if (!persp) { *os = (s16)ss; *ot = (s16)st; return; }
    int wn = sw & 0x7FFF;
    if ((sw & 0x8000) || !wn) wn = 1;
    s64 rcp = (s64)((1ull << 40) / (u32)wn);
    s64 a = ((s64)(s16)ss * rcp) >> 25, b = ((s64)(s16)st * rcp) >> 25;
    *os = a > 32767 ? 32767 : a < -32768 ? -32768 : (s32)a;
    *ot = b > 32767 ? 32767 : b < -32768 ? -32768 : (s32)b;
}

ALWAYS int iabs(int v) { return v < 0 ? -v : v; }

/* Picks the tiles for this pixel and sets the LOD fraction input. */
ALWAYS void lod_select(const int persp, u32 s, u32 t, u32 w, s32 ss, s32 st, int *t0, int *t1) {
    s32 nx, ny, mx, my;
    tcdiv(persp, (s32)(s + P.ds) >> 16, (s32)(t + P.dt) >> 16, (s32)(w + P.dw) >> 16, &nx, &ny);
    tcdiv(persp, (s32)(s + P.dsy) >> 16, (s32)(t + P.dty) >> 16, (s32)(w + P.dwy) >> 16, &mx, &my);
    int lod = iabs(nx - ss), v;
    if ((v = iabs(ny - st)) > lod) lod = v;
    if ((v = iabs(mx - ss)) > lod) lod = v;
    if ((v = iabs(my - st)) > lod) lod = v;
    if (lod > 0x7FFF) lod = 0x7FFF;
    if (M.lod_en && lod < S.prim_lod_min) lod = S.prim_lod_min;
    int magnify = lod < 32;
    int ltile = magnify ? 0 : 31 - __builtin_clz((u32)lod >> 5);
    int distant = ltile >= P.level;
    if (distant) ltile = P.level;
    int frac = ((lod << 3) >> (magnify ? 0 : (31 - __builtin_clz((u32)lod >> 5)))) & 0xFF;
    if (!M.sharpen && !M.detail) {
        if (distant) frac = 0xFF;
        else if (magnify) frac = 0;
    } else if (M.sharpen && magnify) {
        frac |= 0x100;
    }
    s_lod.v = v4s(frac);
    if (!M.lod_en) return;
    int a, b, base = P.tile + ltile;
    if (!M.detail) {
        a = base;
        b = (distant || (!M.sharpen && magnify)) ? a : a + 1;
    } else {
        a = magnify ? base : base + 1;
        b = (!distant && !magnify) ? base + 2 : base + 1;
    }
    *t0 = a & 7;
    *t1 = b & 7;
}

/* ---------------------------------------------------------- span inner loop */
static const u8 magic_m[16] = {0,6,1,7, 4,2,5,3, 3,5,2,4, 7,1,6,0};
static const u8 bayer_m[16] = {0,4,1,5, 4,0,5,1, 3,7,2,6, 7,3,6,2};

static int cov_mask(const Span *sp, int x) {
    int q = x << 2, m = 0;
    for (int r = 0; r < 4; r++) {
        int l = sp->ql[r], h = sp->qr[r], c0 = q + (r & 1), c1 = c0 + 2;
        if (c0 >= l && c0 < h) m |= 0x80 >> (r * 2);
        if (c1 >= l && c1 < h) m |= 0x40 >> (r * 2);
    }
    return m;
}

ALWAYS u32 rnd(void) {
    S.rng = S.rng * 0x343FD + 0x269EC3;
    return S.rng >> 16;
}

ALWAYS void combine_cycle(const CC *cc) {
    v4 raw = (cc->a->v - cc->b->v) * cc->c->v + (cc->d->v << 8) + 0x80;
    s32 al = ((*cc->aa - *cc->ab) * *cc->ac + (*cc->ad << 8) + 0x80) >> 8;
    c_combined.v = vext9(raw >> 8);
    c_combined.c[3] = ext9(al);
    if (M.spl_cm) c_combm.v = ((c_combined.v + 256) & 0x1FF) - 256;   /* MUL input is 9-bit signed */
    if (M.spl_ca) s_comb.v = v4s(((c_combined.c[3] + 256) & 0x1FF) - 256);
}

static int key_alpha(v4 raw) {
    Col c; c.v = raw;
    int ka = 255;
    for (int i = 0; i < 3; i++) {
        int v = sx((u32)c.c[i], 17);
        if (v > 0) v = (v & 15) == 8 ? -v + 16 : -v;
        v += S.key_w[i] << 4;
        if (v < ka) ka = v;
    }
    return ka < 0 ? 0 : ka;
}

ALWAYS v4 blend_eq(const int c, const int div) {
    int a = *M.bA[c] >> 3, b = *M.bB[c] >> 3;
    if (M.special_c[c]) { a = (a >> px_blsa) & 0x3C; b = (b >> px_blsb) | 3; }
    v4 bl = M.bP[c]->v * a + M.bM[c]->v * (b + 1);
    if (div && !M.force_blend) {
        int sum = (a & ~3) + (b & ~3) + 4;
        return __builtin_elementwise_min(bl / v4s(sum), v4s(255));
    }
    return (bl >> 5) & 0xFF;
}

ALWAYS void span_core(const Span *sp, const int two, const int texm, const int zb, const int fb16) {
    const int y = sp->y, x1 = sp->x1, f0 = sp->full0;
    const u32 fw = (u32)(sp->full1 - sp->full0);
    const int bpp = fb16 ? 2 : (S.fb_size == 3 ? 4 : 1);
    const u32 off = (u32)S.fb_width * (u32)y + (u32)sp->x0;
    u32 fa = S.fb_addr + off * (u32)bpp, za = S.z_addr + off * 2;
    v4 rgba = sp->rgba;
    u32 s = sp->s, t = sp->t, w = sp->w, z = sp->z;
    const int persp = texm == 2, dorow = (y & 3) * 4;
    const int use_shade = M.use_shade, aa = M.aa;
    const int dolod = M.use_lod, need0 = M.need0, need1 = M.need1;

    for (int x = sp->x0; x <= x1; x++, fa += (u32)bpp, za += 2,
         rgba += P.drgba, s += P.ds, t += P.dt, w += P.dw, z += P.dz) {
        int cvg = 8, cvbit = 1;
        if ((u32)(x - f0) > fw) {
            int m = cov_mask(sp, x);
            cvg = __builtin_popcount((u32)m);
            cvbit = m >> 7;
            if (!(aa ? cvg : cvbit)) continue;
        }

        /* Z first: cheap, and a reject skips the whole colour pipeline */
        u32 sz = 0, zidx = (za & R_mask & ~1u);
        int infront = 1, nearer = 1, farther = 1, zmax = 0;
        if (zb) {
            if (P.zsel) sz = P.zprim;
            else {
                sz = (z >> 13) & 0x7FFFF;
                switch (sz >> 17) {
                case 2: sz = 0x3FFFF; break;
                case 3: sz = 0; break;
                default: break;
                }
            }
            if (M.z_compare) {
                u32 zv = be16(&R_ram[zidx]);
                u32 oz = z_decompress(zv >> 2);
                int raw = (int)((zv & 3) << 2 | R_hid[zidx >> 1]);
                int dzmem = 1 << raw, force = 0, prec = (int)(zv >> 13);
                if (M.special) {
                    int d = P.dzenc - raw;
                    px_blsa = d < 0 ? 0 : d > 4 ? 4 : d;
                    d = -d;
                    px_blsb = d < 0 ? 0 : d > 4 ? 4 : d;
                }
                if (prec < 3) {
                    if (raw != 15) {
                        int m0 = dzmem << 1, m1 = 16 >> prec;
                        dzmem = m0 > m1 ? m0 : m1;
                    } else {
                        force = 1;
                        dzmem = 0xFFFF;
                    }
                }
                u32 dznew = (u32)(dzmem > P.dzpix ? dzmem : P.dzpix) << 3;
                farther = force || (sz + dznew >= oz);
                nearer = force || (sz <= oz + dznew);
                infront = sz < oz;
                zmax = oz == 0x3FFFF;
                switch (M.z_mode) {
                case 2:  if (!infront && !zmax) continue; break;
                case 3:  if (!(farther && nearer && !zmax)) continue; break;
                default: if (!nearer && !zmax) continue; break;
                }
            }
        }

        /* texture */
        if (texm) {
            s32 ss, st;
            tcdiv(persp, (s32)s >> 16, (s32)t >> 16, (s32)w >> 16, &ss, &st);
            const Tile *tl0 = P.tl0, *tl1 = P.tl1;
            SampleFn fn0 = P.fn0, fn1 = P.fn1;
            if (dolod) {
                int a = P.tile, b = (P.tile + 1) & 7;
                lod_select(persp, s, t, w, ss, st, &a, &b);
                if (M.lod_en) {
                    tl0 = &S.tile[a]; tl1 = &S.tile[b];
                    fn0 = sample_fn[tl0->kind][M.bil];
                    fn1 = sample_fn[tl1->kind][M.bil];
                }
            }
            if (need0) {
                fn0(tl0, ss, st, &c_tex0);
                if (M.spl_t0) s_tex0.v = v4s(c_tex0.c[3]);
                if (M.need_n) { c_texn = c_tex0; s_texn = s_tex0; }
            }
            if (two && need1) {
                fn1(tl1, ss, st, &c_tex1);
                if (M.spl_t1) s_tex1.v = v4s(c_tex1.c[3]);
            }
        }

        if (use_shade) {
            c_shade.v = vclamp255(vext9((rgba >> 16) & 0x1FF));
            if (M.spl_sh) s_shade.v = v4s(c_shade.c[3]);
        }
        if (M.use_noise) {
            int n = (int)(((rnd() & 7) << 6) | 0x20);
            c_noise.v = v4s(n);
        }

        /* combiner */
        if (two) combine_cycle(&M.cc[0]);
        const CC *cc = &M.cc[1];
        v4 raw = (cc->a->v - cc->b->v) * cc->c->v + (cc->d->v << 8) + 0x80;
        s32 al = ext9(((*cc->aa - *cc->ab) * *cc->ac + (*cc->ad << 8) + 0x80) >> 8);
        c_combined.v = vext9(raw >> 8);
        c_combined.c[3] = al;
        if (M.spl_cm) c_combm.v = ((c_combined.v + 256) & 0x1FF) - 256;
        if (M.spl_ca) s_comb.v = v4s(((al + 256) & 0x1FF) - 256);
        c_pixel.v = vclamp255(c_combined.v);

        /* alpha and coverage output stage */
        int a = al < 0 ? 0 : al > 255 ? 255 : al;
        int idx = dorow | (x & 3);
        int adith = M.alpha_dith == 2 ? (int)(rnd() & 7) : M.da[idx];
        int ac;
        if (a == 255) a = 256;
        int tmp = 0;
        if (M.cvg_x_alpha) { tmp = (a * cvg + 4) >> 3; cvg = (tmp >> 5) & 15; }
        if (!M.alpha_cvg) {
            if (M.key) a = key_alpha(raw);
            else { a += adith; if (a & 0x100) a = 255; }
            ac = a;
        } else {
            ac = a > 255 ? 255 : a;
            a = M.cvg_x_alpha ? tmp : cvg << 5;
            if (a > 255) a = 255;
        }
        c_pixel.c[3] = a;
        if (M.acmp) {
            int th = M.dither_alpha ? (int)(rnd() & 0xFF) : P.acthresh;
            if (ac < th) continue;
        }
        if (!(aa ? cvg : cvbit)) continue;
        if (M.use_shade_bl) {
            int sa = c_shade.c[3] + adith;
            px_shade_a = sa > 255 ? 255 : sa;
        }

        /* memory read */
        u32 fidx = fa & R_mask & (fb16 ? ~1u : ~0u);
        int memcvg = 7;
        if (M.mem_always || cvg < 8) {
            if (fb16) {
                u32 fv = be16(&R_ram[fidx]);
                c_mem.v = (v4){(s32)(fv >> 8) & 0xF8, (s32)(fv >> 3) & 0xF8, (s32)(fv << 2) & 0xF8, 0xE0};
                if (M.image_read) {
                    memcvg = (int)((fv & 1) << 2 | R_hid[fidx >> 1]);
                    c_mem.c[3] = memcvg << 5;
                }
            } else if (bpp == 4) {
                const u8 *p = &R_ram[fidx & ~3u];
                c_mem.v = (v4){p[0], p[1], p[2], 0xE0};
                if (M.image_read) { c_mem.c[3] = p[3]; memcvg = p[3] >> 5; }
            } else {
                int v = R_ram[fidx];
                c_mem.v = (v4){v, v, v, 0xE0};
            }
        }

        /* overflow, blend enable, final depth decision */
        int overflow = ((memcvg + cvg) & 8) != 0;
        int blend_en = M.force_blend || (!overflow && aa && farther);
        if (zb && M.z_compare && M.z_mode < 2 && !zmax && !(overflow ? infront : nearer)) continue;

        /* blender */
        v4 out;
        if (two) {
            px_inv = ~*M.bA[0] & 0xFF;
            c_blended.v = blend_eq(0, 0);
        }
        const int bc = two ? 1 : 0;
        if (!M.color_on_cvg || overflow) {
            if (!blend_en || (M.partial && a >= 255)) out = M.bP[bc]->v;
            else {
                px_inv = ~*M.bA[bc] & 0xFF;
                out = blend_eq(bc, 1);
            }
        } else {
            out = M.bM[bc]->v;
        }

        if (fb16 && M.rgb_dith != 3) {
            v4 dv;
            if (M.rgb_dith == 2) { u32 n = rnd(); dv = (v4){(s32)(n & 7), (s32)((n >> 3) & 7), (s32)((n >> 6) & 7), 0}; }
            else dv = v4s(M.dr[idx]);
            v4 nw = __builtin_elementwise_min((out & 0xF8) + 8, v4s(255));
            v4 mk = (out & 7) > dv;
            out = (nw & mk) | (out & ~mk);
        }

        /* coverage to memory */
        int fc;
        switch (M.cvg_dest) {
        case 0:
            if (!blend_en) fc = (cvg - 1) & 7;
            else { fc = memcvg + cvg; fc = (fc & 8) ? 7 : fc & 7; }
            break;
        case 1:  fc = (memcvg + cvg) & 7; break;
        case 2:  fc = 7; break;
        default: fc = memcvg; break;
        }

        Col oc; oc.v = out;
        if (fb16) {
            u32 r = (u32)oc.c[0], g = (u32)oc.c[1], b = (u32)oc.c[2];
            wbe16(&R_ram[fidx], (u16)(((r & 0xF8) << 8) | ((g & 0xF8) << 3) | ((b & 0xF8) >> 2) | ((u32)fc >> 2)));
            R_hid[fidx >> 1] = (u8)(fc & 3);
        } else if (bpp == 4) {
            u8 *p = &R_ram[fidx & ~3u];
            p[0] = (u8)oc.c[0]; p[1] = (u8)oc.c[1]; p[2] = (u8)oc.c[2]; p[3] = (u8)(fc << 5);
        } else {
            R_ram[fidx] = (u8)oc.c[0];
        }
        if (zb && M.z_update) {
            wbe16(&R_ram[zidx], (u16)((z_compress(sz) << 2) | ((u32)P.dzenc >> 2)));
            R_hid[zidx >> 1] = (u8)(P.dzenc & 3);
        }
    }
}

#define SPF(tw, tm, zb, fb) \
    static void sp_##tw##tm##zb##fb(const Span *s) { span_core(s, tw, tm, zb, fb); }
#define SPF2(tw, tm, zb) SPF(tw, tm, zb, 0) SPF(tw, tm, zb, 1)
#define SPF3(tw, tm) SPF2(tw, tm, 0) SPF2(tw, tm, 1)
#define SPF4(tw) SPF3(tw, 0) SPF3(tw, 1) SPF3(tw, 2)
SPF4(0) SPF4(1)
#define SPE2(tw, tm, zb) { sp_##tw##tm##zb##0, sp_##tw##tm##zb##1 }
#define SPE3(tw, tm) { SPE2(tw, tm, 0), SPE2(tw, tm, 1) }
#define SPE4(tw) { SPE3(tw, 0), SPE3(tw, 1), SPE3(tw, 2) }
static const SpanFn span_tab[2][3][2][2] = { SPE4(0), SPE4(1) };

/* --------------------------------------------------------------- mode setup */
static void col_from(Col *c, u32 v) {
    c->c[0] = (s32)(v >> 24);
    c->c[1] = (s32)((v >> 16) & 0xFF);
    c->c[2] = (s32)((v >> 8) & 0xFF);
    c->c[3] = (s32)(v & 0xFF);
}

static const Col *sel_abd(int v, int stage, const Col *ta, const Col *tb) {
    switch (v) {
    case 0: return &c_combined;
    case 1: return ta;
    case 2: return tb;
    case 3: return &c_prim;
    case 4: return &c_shade;
    case 5: return &c_env;
    case 6: return stage == 1 ? &c_keyc : &c_one;
    case 7: return stage == 0 ? &c_noise : stage == 1 ? &c_k4 : &c_zero;
    default: return &c_zero;
    }
}
static const Col *sel_mul(int v, const Col *ta, const Col *tb, const Col *sa, const Col *sb) {
    switch (v) {
    case 0: return &c_combm;
    case 1: return ta;
    case 2: return tb;
    case 3: return &c_prim;
    case 4: return &c_shade;
    case 5: return &c_env;
    case 6: return &c_keys;
    case 7: return &s_comb;
    case 8: return sa;
    case 9: return sb;
    case 10: return &s_prim;
    case 11: return &s_shade;
    case 12: return &s_env;
    case 13: return &s_lod;
    case 14: return &s_plod;
    case 15: return &c_k5;
    default: return &c_zero;
    }
}
static const s32 *sel_a(int v, const Col *ta, const Col *tb) {
    switch (v) {
    case 0: return &c_combined.c[3];
    case 1: return &ta->c[3];
    case 2: return &tb->c[3];
    case 3: return &c_prim.c[3];
    case 4: return &c_shade.c[3];
    case 5: return &c_env.c[3];
    case 6: return &c_one.c[3];
    default: return &c_zero.c[3];
    }
}
static const s32 *sel_am(int v, const Col *ta, const Col *tb) {
    switch (v) {
    case 0: return &s_lod.c[3];
    case 1: return &ta->c[3];
    case 2: return &tb->c[3];
    case 3: return &c_prim.c[3];
    case 4: return &c_shade.c[3];
    case 5: return &c_env.c[3];
    case 6: return &s_plod.c[3];
    default: return &c_zero.c[3];
    }
}

static void build_cc(int i, const Col *ta, const Col *tb, const Col *sa, const Col *sb) {
    u32 hi = HI(S.combine), lo = LO(S.combine);
    CC *c = &M.cc[i];
    if (i == 0) {
        c->a = sel_abd((hi >> 20) & 15, 0, ta, tb);
        c->c = sel_mul((hi >> 15) & 31, ta, tb, sa, sb);
        c->aa = sel_a((hi >> 12) & 7, ta, tb);
        c->ac = sel_am((hi >> 9) & 7, ta, tb);
        c->b = sel_abd((lo >> 28) & 15, 1, ta, tb);
        c->d = sel_abd((lo >> 15) & 7, 2, ta, tb);
        c->ab = sel_a((lo >> 12) & 7, ta, tb);
        c->ad = sel_a((lo >> 9) & 7, ta, tb);
    } else {
        c->a = sel_abd((hi >> 5) & 15, 0, ta, tb);
        c->c = sel_mul(hi & 31, ta, tb, sa, sb);
        c->b = sel_abd((lo >> 24) & 15, 1, ta, tb);
        c->aa = sel_a((lo >> 21) & 7, ta, tb);
        c->ac = sel_am((lo >> 18) & 7, ta, tb);
        c->d = sel_abd((lo >> 6) & 7, 2, ta, tb);
        c->ab = sel_a((lo >> 3) & 7, ta, tb);
        c->ad = sel_a(lo & 7, ta, tb);
    }
}

static int cc_ref(const CC *c, const Col *p) {
    return c->a == p || c->b == p || c->c == p || c->d == p ||
           c->aa == &p->c[3] || c->ab == &p->c[3] || c->ac == &p->c[3] || c->ad == &p->c[3];
}

static void build_blender(int i, u32 lo) {
    int p = (lo >> (30 - i * 2)) & 3, a = (lo >> (26 - i * 2)) & 3;
    int m = (lo >> (22 - i * 2)) & 3, b = (lo >> (18 - i * 2)) & 3;
    const Col *in = i == 0 ? &c_pixel : &c_blended;
    const Col *tab[4] = {in, &c_mem, &c_blend, &c_fog};
    M.bP[i] = tab[p];
    M.bM[i] = tab[m];
    M.bA[i] = a == 0 ? &c_pixel.c[3] : a == 1 ? &c_fog.c[3] : a == 2 ? &px_shade_a : &k_0;
    M.bB[i] = b == 0 ? &px_inv : b == 1 ? &c_mem.c[3] : b == 2 ? &k_255 : &k_0;
}

static void update_modes(void) {
    u32 hi = HI(S.om), lo = LO(S.om);
    M.om = S.om; M.comb = S.combine; M.valid = 1;
    M.cycle = (hi >> 20) & 3;
    M.two = M.cycle == 1;
    M.persp = (hi >> 19) & 1;
    M.detail = (hi >> 18) & 1;
    M.sharpen = (hi >> 17) & 1;
    M.lod_en = (hi >> 16) & 1;
    M.tlut = (hi >> 15) & 1;
    M.tlut_ia = (hi >> 14) & 1;
    M.bil = (hi >> 13) & 1;
    M.mid = (hi >> 12) & 1;
    M.key = (hi >> 8) & 1;
    M.rgb_dith = (hi >> 6) & 3;
    M.alpha_dith = (hi >> 4) & 3;
    M.force_blend = (lo >> 14) & 1;
    M.alpha_cvg = (lo >> 13) & 1;
    M.cvg_x_alpha = (lo >> 12) & 1;
    M.z_mode = (lo >> 10) & 3;
    M.cvg_dest = (lo >> 8) & 3;
    M.color_on_cvg = (lo >> 7) & 1;
    M.image_read = (lo >> 6) & 1;
    M.z_update = (lo >> 5) & 1;
    M.z_compare = (lo >> 4) & 1;
    M.aa = (lo >> 3) & 1;
    M.z_sel = (lo >> 2) & 1;
    M.dither_alpha = (lo >> 1) & 1;
    M.acmp = lo & 1;

    const int two = M.two;
    if (two) {
        build_cc(0, &c_tex0, &c_tex1, &s_tex0, &s_tex1);
        build_cc(1, &c_tex1, &c_texn, &s_tex1, &s_texn);
    } else {
        build_cc(1, &c_tex0, &c_tex0, &s_tex0, &s_tex0);
        M.cc[0] = M.cc[1];
    }
    const CC *c0 = &M.cc[0], *c1 = &M.cc[1];
#define REF(p) (cc_ref(c1, &(p)) || (two && cc_ref(c0, &(p))))
    M.need_n = REF(c_texn) || REF(s_texn);
    M.need0 = REF(c_tex0) || REF(s_tex0) || M.need_n;
    M.need1 = two && (REF(c_tex1) || REF(s_tex1));
    M.use_noise = REF(c_noise);
    M.use_lod = M.lod_en || REF(s_lod);
    M.spl_t0 = REF(s_tex0) || REF(s_texn);
    M.spl_t1 = REF(s_tex1);
    M.spl_sh = REF(s_shade);
    M.spl_cm = REF(c_combm);
    M.spl_ca = REF(s_comb);
    M.use_shade = REF(c_shade) || REF(s_shade);
#undef REF

    build_blender(0, lo);
    build_blender(1, lo);
    int bc = two ? 1 : 0;
    M.use_shade_bl = M.bA[0] == &px_shade_a || (two && M.bA[1] == &px_shade_a);
    if (M.use_shade_bl) M.use_shade = 1;
    M.partial = M.bB[bc] == &px_inv && M.bA[bc] == &c_pixel.c[3];
    M.special_c[0] = M.bB[0] == &c_mem.c[3];
    M.special_c[1] = M.bB[1] == &c_mem.c[3];
    M.special = M.special_c[0] || (two && M.special_c[1]);
    M.mem_always = M.force_blend || M.cvg_dest == 1 || M.cvg_dest == 3 || M.bP[bc] == &c_mem ||
                   (two && (M.bP[0] == &c_mem || M.bM[0] == &c_mem || M.bB[0] == &c_mem.c[3]));

    /* rgb and alpha dither patterns */
    const u8 *pat = M.rgb_dith == 1 ? bayer_m : magic_m;
    for (int i = 0; i < 16; i++) {
        M.dr[i] = pat[i];
        M.da[i] = M.alpha_dith == 0 ? pat[i] : M.alpha_dith == 1 ? (u8)(~pat[i] & 7) : 0;
    }
}

/* Called before each primitive: syncs everything derived from S. */
static void prep(int tile, int level) {
    if (!M.valid || M.om != S.om || M.comb != S.combine) update_modes();
    /* per-pixel leftovers aren't state: start each primitive clean so replays match */
    c_combined = c_combm = c_tex0 = c_tex1 = c_texn = c_shade = c_noise = c_zero;
    s_comb = s_tex0 = s_tex1 = s_texn = s_shade = c_pixel = c_blended = c_mem = c_zero;
    px_shade_a = px_inv = px_blsa = px_blsb = 0;
    memset(&P, 0, sizeof P);
    col_from(&c_prim, S.prim);
    col_from(&c_env, S.env);
    col_from(&c_blend, S.blend);
    col_from(&c_fog, S.fog);
    s_prim.v = v4s(c_prim.c[3]);
    s_env.v = v4s(c_env.c[3]);
    s_plod.v = v4s(S.prim_lod_frac);
    s_lod.v = v4s(0);
    c_one.v = v4s(256);
    c_zero.v = v4s(0);
    for (int i = 0; i < 3; i++) { c_keyc.c[i] = S.key_c[i]; c_keys.c[i] = S.key_s[i]; }
    c_k4.v = v4s(S.k[4]);
    c_k5.v = v4s(S.k[5]);
    for (int i = 0; i < 8; i++) S.tile[i].kind = (u8)tile_kind(&S.tile[i]);
    P.tile = tile;
    P.level = level;
    P.tl0 = &S.tile[tile];
    P.tl1 = &S.tile[(tile + 1) & 7];
    P.fn0 = sample_fn[P.tl0->kind][M.bil];
    P.fn1 = sample_fn[P.tl1->kind][M.bil];
    P.acthresh = (int)(S.blend & 0xFF);
    P.zsel = M.z_sel;
    P.zprim = (u32)S.prim_z << 3;
}

static SpanFn pick_span(int texm) {
    int zb = M.z_compare || M.z_update;
    return span_tab[M.two][(M.need0 || M.need1 || M.use_lod) ? texm : 0][zb][S.fb_size == 2];
}

/* ---------------------------------------------------------------- triangles */
ALWAYS s32 mk(u64 ints, u64 fracs, int lane) {
    int sh = 48 - 16 * lane;
    return (s32)((((u32)(ints >> sh) & 0xFFFF) << 16) | ((u32)(fracs >> sh) & 0xFFFF));
}

static void draw_tri(const u64 *w, int shade, int tex, int zb) {
    u32 hi = HI(w[0]), lo = LO(w[0]);
    int flip = (hi >> 23) & 1;
    prep((hi >> 16) & 7, (hi >> 19) & 7);

    s32 yl = sx(hi & 0x3FFF, 14), ym = sx(lo >> 16, 14), yh = sx(lo, 14);
    u32 xl = (u32)sx(HI(w[1]), 30), xh0 = (u32)sx(HI(w[2]), 30), xm0 = (u32)sx(HI(w[3]), 30);
    const s32 dxl_s = sx(LO(w[1]), 30), dxh_s = sx(LO(w[2]), 30), dxm_s = sx(LO(w[3]), 30);
    const u32 dxl = (u32)((dxl_s >> 2) & ~1), dxh = (u32)((dxh_s >> 2) & ~1), dxm = (u32)((dxm_s >> 2) & ~1);

    /* attribute planes */
    v4 v0 = v4s(0), de = v4s(0), dy = v4s(0);
    P.drgba = v4s(0);
    const u64 *b = w + 4;
    u32 s0 = 0, t0 = 0, w0 = 0, z0 = 0;
    s32 dsde = 0, dtde = 0, dwde = 0, dzde = 0, dsdy = 0, dtdy = 0, dwdy = 0, dzdy = 0;
    s32 dsdx = 0, dtdx = 0, dwdx = 0, dzdx = 0;
    if (shade) {
        Col c;
#define LANES(dst, A, B) for (int i = 0; i < 4; i++) c.c[i] = mk(A, B, i); dst = c.v
        LANES(v0, b[0], b[2]);
        LANES(P.drgba, b[1], b[3]);
        LANES(de, b[4], b[6]);
        LANES(dy, b[5], b[7]);
#undef LANES
        b += 8;
    }
    if (tex) {
        s0 = (u32)mk(b[0], b[2], 0); t0 = (u32)mk(b[0], b[2], 1); w0 = (u32)mk(b[0], b[2], 2);
        dsdx = mk(b[1], b[3], 0); dtdx = mk(b[1], b[3], 1); dwdx = mk(b[1], b[3], 2);
        dsde = mk(b[4], b[6], 0); dtde = mk(b[4], b[6], 1); dwde = mk(b[4], b[6], 2);
        dsdy = mk(b[5], b[7], 0); dtdy = mk(b[5], b[7], 1); dwdy = mk(b[5], b[7], 2);
        b += 8;
    }
    if (zb) {
        z0 = HI(b[0]); dzdx = (s32)LO(b[0]);
        dzde = (s32)HI(b[1]); dzdy = (s32)LO(b[1]);
    }
    if (hr_n > 1) {
        /* Edges keep their slopes when x and y scale together. Attribute steps
         * shrink, and the start values move to the first high-res scanline. */
        const int n = hr_n, e = ((yh * n) >> 2) - (yh >> 2) * n;
        xh0 = xh0 * (u32)n + (u32)e * (u32)dxh_s;
        xm0 = xm0 * (u32)n + (u32)e * (u32)dxm_s;
        xl *= (u32)n; yl *= n; ym *= n; yh *= n;
        P.drgba = P.drgba / v4s(n); de = de / v4s(n); dy = dy / v4s(n);
        dsdx /= n; dtdx /= n; dwdx /= n; dzdx /= n;
        dsde /= n; dtde /= n; dwde /= n; dzde /= n;
        dsdy /= n; dtdy /= n; dwdy /= n; dzdy /= n;
        v0 += de * e;
        s0 += (u32)(dsde * e); t0 += (u32)(dtde * e); w0 += (u32)(dwde * e); z0 += (u32)(dzde * e);
    }
    P.ds = (u32)dsdx; P.dt = (u32)dtdx; P.dw = (u32)dwdx; P.dz = (u32)dzdx;
    P.dsy = (u32)dsdy; P.dty = (u32)dtdy; P.dwy = (u32)dwdy;
    if (P.zsel) {
        P.dzpix = S.prim_dz;
        P.dzenc = dz_encode(S.prim_dz);
    } else {
        int a = (dzdx >> 16) & 0xFFFF, c = (dzdy >> 16) & 0xFFFF;
        a = (a & 0x8000) ? (~a & 0x7FFF) : a;
        c = (c & 0x8000) ? (~c & 0x7FFF) : c;
        P.dzpix = dz_normalize(a + c);
        P.dzenc = dz_encode(P.dzpix);
    }

    /* Which sub-scanline samples the major edge for the attribute adjustment,
     * and the 3/4-scanline correction that goes with it. */
    const int ldflag = ((dxh_s < 0) ^ flip) ? 0 : 3;
    v4 diff = v4s(0);
    u32 sd = 0, td = 0, wd = 0, zd = 0;
    if (ldflag) {
        diff = de - (de >> 2) - dy + (dy >> 2);
        sd = (u32)(dsde - (dsde >> 2) - dsdy + (dsdy >> 2));
        td = (u32)(dtde - (dtde >> 2) - dtdy + (dtdy >> 2));
        wd = (u32)(dwde - (dwde >> 2) - dwdy + (dwdy >> 2));
        zd = (u32)(dzde - (dzde >> 2) - dzdy + (dzdy >> 2));
    }
    const v4 drx8 = P.drgba >> 8;
    const s32 dsx8 = dsdx >> 8, dtx8 = dtdx >> 8, dwx8 = dwdx >> 8, dzx8 = dzdx >> 8;

    /* vertical range */
    const int ys = yh >> 2;
    const int kend = yl < S.sc_yl ? yl : S.sc_yl;
    int y = ys, ylast = (kend - 1) >> 2;
    if (y < (S.sc_yh >> 2)) y = S.sc_yh >> 2;
    if (y > ylast) return;

    int k = y * 4, kstart = ys * 4;
    u32 xh = xh0 + (u32)(k - kstart) * dxh, xm, dxminor;
    if (k >= ym) { xm = xl + (u32)(k - ym) * dxl; dxminor = dxl; }
    else         { xm = xm0 + (u32)(k - kstart) * dxm; dxminor = dxm; }

    const SpanFn fn = pick_span((M.persp && tex) ? 2 : 1);
    const int n0 = y - ys;
    v4 ca = v0 + de * n0;
    u32 cs = s0 + (u32)n0 * (u32)dsde, ct = t0 + (u32)n0 * (u32)dtde;
    u32 cw = w0 + (u32)n0 * (u32)dwde, cz = z0 + (u32)n0 * (u32)dzde;
    const int sxl = S.sc_xh, sxr = S.sc_xl;

    for (; y <= ylast; y++, ca += de, cs += (u32)dsde, ct += (u32)dtde, cw += (u32)dwde, cz += (u32)dzde) {
        Span sp;
        int minl = 0x7FFF, maxr = -0x7FFF, maxl = -0x7FFF, minr = 0x7FFF, valid = 0;
        u32 xmaj = xh;
        for (int r = 0; r < 4; r++, k++) {
            if (k == ym) { xm = xl; dxminor = dxl; }
            if (r == ldflag) xmaj = xh;
            int q0 = (s32)(flip ? xh : xm) >> 14, q1 = (s32)(flip ? xm : xh) >> 14;
            if (q0 < sxl) q0 = sxl;
            if (q0 > sxr) q0 = sxr;
            if (q1 < sxl) q1 = sxl;
            if (q1 > sxr) q1 = sxr;
            if (k >= yh && k < kend && k >= S.sc_yh && q0 < q1) {
                sp.ql[r] = (s16)q0; sp.qr[r] = (s16)q1;
                valid++;
                if (q0 < minl) minl = q0;
                if (q1 > maxr) maxr = q1;
                if (q0 > maxl) maxl = q0;
                if (q1 < minr) minr = q1;
            } else {
                sp.ql[r] = 0x7FFF; sp.qr[r] = -0x7FFF;
            }
            xh += dxh;
            xm += dxminor;
        }
        if (!valid) continue;
        if (S.sc_field && (y & 1) != S.sc_odd) continue;

        sp.y = y;
        sp.x0 = minl >> 2;
        sp.x1 = (maxr - 1) >> 2;
        if (sp.x0 > sp.x1) continue;
        if (valid == 4) {
            sp.full0 = (maxl + 3) >> 2;
            sp.full1 = (minr >> 2) - 1;
        } else {
            sp.full0 = 1; sp.full1 = 0;
        }
        if (sp.full1 < sp.full0) { sp.full0 = 0x7FFFFFF; sp.full1 = 0x7FFFFFF; }

        s32 xd = (sp.x0 << 8) - ((s32)xmaj >> 8);
        sp.rgba = ca + diff + drx8 * xd;
        sp.s = cs + sd + (u32)xd * (u32)dsx8;
        sp.t = ct + td + (u32)xd * (u32)dtx8;
        sp.w = cw + wd + (u32)xd * (u32)dwx8;
        sp.z = cz + zd + (u32)xd * (u32)dzx8;
        fn(&sp);
    }
}

/* --------------------------------------------------------------- rectangles */
static void fill_mode_rect(int x0, int y0, int x1, int y1) {
    const u32 fill = S.fill, step = S.fb_size == 3 ? 4 : S.fb_size == 2 ? 2 : 1;
    const u16 hc = (u16)(fill >> 16), lc = (u16)fill;
    const u8 hh = (hc & 1) ? 3 : 0, lh = (lc & 1) ? 3 : 0;
    for (int y = y0; y <= y1; y++) {
        if (S.sc_field && (y & 1) != S.sc_odd) continue;
        u32 a = S.fb_addr + ((u32)S.fb_width * (u32)y + (u32)x0) * step;
        for (int x = x0; x <= x1; x++, a += step) {
            if (step == 2) {
                u32 m = a & R_mask & ~1u, up = !(a & 2);
                wbe16(&R_ram[m], up ? hc : lc);
                R_hid[m >> 1] = up ? hh : lh;
            } else if (step == 4) {
                u32 m = a & R_mask & ~3u;
                R_ram[m] = (u8)(fill >> 24); R_ram[m + 1] = (u8)(fill >> 16);
                R_ram[m + 2] = (u8)(fill >> 8); R_ram[m + 3] = (u8)fill;
            } else {
                R_ram[a & R_mask] = (u8)(fill >> (8 * (3 - (a & 3))));
            }
        }
    }
}

static void copy_line(int y, int x0, int x1, u32 s, u32 t, u32 ds, u32 dt) {
    const SampleFn fn = sample_fn[P.tl0->kind][0];
    const int is16 = S.fb_size == 2, acmp = M.acmp;
    u32 a = S.fb_addr + ((u32)S.fb_width * (u32)y + (u32)x0) * (is16 ? 2u : S.fb_size == 3 ? 4u : 1u);
    for (int x = x0; x <= x1; x++, s += ds, t += dt) {
        Col c;
        fn(P.tl0, (s32)s >> 16, (s32)t >> 16, &c);
        int bit = c.c[3] >= 128;
        u32 m;
        if (is16) {
            m = a & R_mask & ~1u; a += 2;
            if (acmp && !bit) continue;
            u32 v = ((u32)c.c[0] & 0xF8) << 8 | ((u32)c.c[1] & 0xF8) << 3 | ((u32)c.c[2] & 0xF8) >> 2 | (u32)bit;
            wbe16(&R_ram[m], (u16)v);
            R_hid[m >> 1] = bit ? 3 : 0;
        } else if (S.fb_size == 3) {
            m = a & R_mask & ~3u; a += 4;
            if (acmp && !bit) continue;
            for (int i = 0; i < 4; i++) R_ram[m + (u32)i] = (u8)c.c[i];
        } else {
            m = a & R_mask; a += 1;
            if (acmp && !bit) continue;
            R_ram[m] = (u8)c.c[0];
        }
    }
}

/* xh,yh top-left and xl,yl bottom-right in 10.2. s,t S10.5; dsdx,dtdy S5.10. */
static void draw_rect(int xh, int yh, int xl, int yl, int tile,
                      s32 s, s32 t, s32 dsdx, s32 dtdy, int flip) {
    prep(tile, 0);
    const int inclusive = M.cycle >= 2;
    int x0, x1, y0, y1;
    if (inclusive) { x0 = xh >> 2; x1 = xl >> 2; y0 = yh >> 2; y1 = yl >> 2; }
    else { x0 = (xh + 3) >> 2; x1 = ((xl + 3) >> 2) - 1; y0 = (yh + 3) >> 2; y1 = ((yl + 3) >> 2) - 1; }
    const int cx0 = (S.sc_xh + 3) >> 2, cx1 = ((S.sc_xl + 3) >> 2) - 1;
    const int cy0 = (S.sc_yh + 3) >> 2, cy1 = ((S.sc_yl + 3) >> 2) - 1;
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (x0 > x1 || y0 > y1) return;
    const int nx0 = x0, ny0 = y0, n = hr_n;
    if (n > 1) { x0 *= n; y0 *= n; x1 = (x1 + 1) * n - 1; y1 = (y1 + 1) * n - 1; }

    if (M.cycle == 3) { fill_mode_rect(x0, y0, x1, y1); return; }

    u32 sb = (u32)s << 16, tb = (u32)t << 16;
    s32 dxa = dsdx * (1 << 11), dya = dtdy * (1 << 11);   /* per pixel, 16.16 of S10.5 */
    s32 dsx = flip ? 0 : dxa, dtx = flip ? dya : 0, dsy = flip ? dxa : 0, dty = flip ? 0 : dya;
    if (M.cycle == 2) { dsx /= 4; dtx /= 4; }
    /* the plane starts at the (possibly fractional) left edge, top scanline */
    s32 xq = (nx0 << 2) - xh;
    u32 sl = sb + (u32)(((s64)dsx * xq) >> 2) + (u32)(ny0 - (yh >> 2)) * (u32)dsy;
    u32 tl = tb + (u32)(((s64)dtx * xq) >> 2) + (u32)(ny0 - (yh >> 2)) * (u32)dty;
    dsx /= n; dtx /= n; dsy /= n; dty /= n;

    if (M.cycle == 2) {
        for (int y = y0; y <= y1; y++, sl += (u32)dsy, tl += (u32)dty) {
            if (S.sc_field && (y & 1) != S.sc_odd) continue;
            copy_line(y, x0, x1, sl, tl, (u32)dsx, (u32)dtx);
        }
        return;
    }

    P.drgba = v4s(0);
    P.ds = (u32)dsx; P.dt = (u32)dtx; P.dw = 0; P.dz = 0;
    P.dsy = (u32)dsy; P.dty = (u32)dty; P.dwy = 0;
    P.dzpix = P.zsel ? S.prim_dz : 1;
    P.dzenc = dz_encode(P.dzpix);
    const SpanFn fn = pick_span(1);
    Span sp;
    memset(&sp, 0, sizeof sp);
    sp.x0 = sp.full0 = x0;
    sp.x1 = sp.full1 = x1;
    for (int y = y0; y <= y1; y++, sl += (u32)dsy, tl += (u32)dty) {
        if (S.sc_field && (y & 1) != S.sc_odd) continue;
        sp.y = y; sp.s = sl; sp.t = tl;
        fn(&sp);
    }
}

/* ------------------------------------------------------------- texture loads */
static void tile_derive(Tile *t) {
    t->clamp_s = t->cs || !t->mask_s;
    t->clamp_t = t->ct || !t->mask_t;
    t->m_s = t->mask_s > 10 ? 10 : t->mask_s;
    t->m_t = t->mask_t > 10 ? 10 : t->mask_t;
    t->wrap_s = (u16)((1 << t->m_s) - 1);
    t->wrap_t = (u16)((1 << t->m_t) - 1);
    t->cds = (u16)(((t->sh >> 2) - (t->sl >> 2)) & 0x3FF);
    t->cdt = (u16)(((t->th >> 2) - (t->tl >> 2)) & 0x3FF);
}

static void tile_set_size(Tile *t, u32 hi, u32 lo) {
    t->sl = (hi >> 12) & 0xFFF; t->tl = hi & 0xFFF;
    t->sh = (lo >> 12) & 0xFFF; t->th = lo & 0xFFF;
    tile_derive(t);
}

ALWAYS void tmem_w32(u32 p16, u32 src) {   /* a 32-bit texel straddles both banks */
    u32 a = (p16 & 0x3FF) * 2;
    S.tmem[a] = n64_rdram[src & AMASK];
    S.tmem[a + 1] = n64_rdram[(src + 1) & AMASK];
    S.tmem[a | 0x800] = n64_rdram[(src + 2) & AMASK];
    S.tmem[(a | 0x800) + 1] = n64_rdram[(src + 3) & AMASK];
}

static void load_block(u32 hi, u32 lo) {
    Tile *tl = &S.tile[(lo >> 24) & 7];
    u32 sl = (hi >> 12) & 0xFFF, ttl = hi & 0xFFF, sh = (lo >> 12) & 0xFFF, dxt = lo & 0xFFF;
    tile_set_size(tl, hi, lo);
    if (sh < sl) return;
    u32 texels = sh - sl + 1, size = (u32)S.ti_size;
    u32 src = S.ti_addr + ((((u32)S.ti_width * ttl + sl) << size) >> 1);
    u32 line = tl->line;
    if (size == 3) {
        if (texels > 1024) texels = 1024;
        for (u32 n = 0; n < texels; n++, src += 4) {
            u32 row = ((n >> 1) * dxt) >> 11;
            tmem_w32((u32)tl->tmem * 4 + row * line * 4 + n, src);
        }
        return;
    }
    u32 words = ((((texels << size) >> 1)) + 7) >> 3;
    if (words > 512) words = 512;
    for (u32 i = 0; i < words; i++, src += 8) {
        u32 row = (i * dxt) >> 11;
        u32 d = (((u32)tl->tmem + i + row * line) & 0x1FF) * 8;
        for (u32 k = 0; k < 8; k++) S.tmem[d + k] = n64_rdram[(src + k) & AMASK];
    }
}

static void load_tile(u32 hi, u32 lo) {
    Tile *tl = &S.tile[(lo >> 24) & 7];
    tile_set_size(tl, hi, lo);
    int x0 = tl->sl >> 2, y0 = tl->tl >> 2, x1 = tl->sh >> 2, y1 = tl->th >> 2;
    u32 size = (u32)S.ti_size;
    for (int y = y0; y <= y1; y++) {
        u32 src = S.ti_addr + ((((u32)S.ti_width * (u32)y + (u32)x0) << size) >> 1);
        u32 row = (u32)(y - y0) * tl->line;
        if (size == 3) {
            for (int x = x0; x <= x1; x++, src += 4)
                tmem_w32(((u32)tl->tmem + row) * 4 + (u32)(x - x0), src);
        } else {
            u32 bytes = ((u32)(x1 - x0 + 1) << size) >> 1;
            u32 d = ((u32)tl->tmem + row) * 8;
            if (bytes > 4096) bytes = 4096;
            for (u32 i = 0; i < bytes; i++) S.tmem[(d + i) & 0xFFF] = n64_rdram[(src + i) & AMASK];
        }
    }
}

static void load_tlut(u32 hi, u32 lo) {
    Tile *tl = &S.tile[(lo >> 24) & 7];
    tile_set_size(tl, hi, lo);
    int s0 = tl->sl >> 2, s1 = tl->sh >> 2, t0 = tl->tl >> 2;
    u32 src = S.ti_addr + (((u32)S.ti_width * (u32)t0 + (u32)s0) << 1);
    u32 d = (u32)tl->tmem * 8;
    for (int i = s0; i <= s1; i++, src += 2, d += 8) {
        u8 h = n64_rdram[src & AMASK], l = n64_rdram[(src + 1) & AMASK];
        for (u32 k = 0; k < 8; k += 2) {       /* each bank holds a copy */
            S.tmem[(d + k) & 0xFFF] = h;
            S.tmem[(d + k + 1) & 0xFFF] = l;
        }
    }
}

/* --------------------------------------------------------------- dispatcher */
static const u8 cmd_len[64] = {
    [0x08] = 4, [0x09] = 6, [0x0A] = 12, [0x0B] = 14, [0x0C] = 12, [0x0D] = 14, [0x0E] = 20, [0x0F] = 22,
    [0x24] = 2, [0x25] = 2,
};

/* ------------------------------------------------------- high-res rendering */
#define HR_POOL  0x10000000u       /* colour is 32-bit, depth 16-bit */
#define HR_SLOTS 8
static u8 *hr_mem, *hr_hid;
static int hr_want = 1;
static struct { u32 addr, stamp; int width; } hr_slot[HR_SLOTS];
static u32 hr_clock;

void rdp_set_hires(int n) {
    if (n == hr_want) return;
    hr_want = n;
    memset(hr_slot, 0, sizeof hr_slot);
    if (n > 1 && !hr_mem) {        /* untouched pages cost nothing */
        hr_mem = calloc(HR_POOL, 1);
        hr_hid = calloc(HR_POOL / 2, 1);
    }
}

/* Offset in the pool of the image the game keeps at RDRAM address `addr`. */
static u32 hr_image(u32 addr, int width) {
    int best = 0;
    for (int i = 0; i < HR_SLOTS; i++) {
        if (hr_slot[i].stamp && hr_slot[i].addr == addr) { hr_slot[i].stamp = ++hr_clock; return (u32)i * (HR_POOL / HR_SLOTS); }
        if (hr_slot[i].stamp < hr_slot[best].stamp) best = i;
    }
    hr_slot[best].addr = addr; hr_slot[best].width = width; hr_slot[best].stamp = ++hr_clock;
    return (u32)best * (HR_POOL / HR_SLOTS);
}

static void exec_cmd(const u64 *w);

static void hr_replay(const u64 *w, int tri) {
    /* smaller targets are textures the game renders for itself */
    if (S.fb_size < 2 || S.fb_width < 200) return;
    const int n = hr_want;
    const u32 fa = S.fb_addr, za = S.z_addr, fill = S.fill, rng = S.rng;
    const int fw = S.fb_width, fs = S.fb_size, xh = S.sc_xh, yh = S.sc_yh, xl = S.sc_xl, yl = S.sc_yl, sf = S.sc_field;
    const int depth = fa == za;
    S.fb_addr = hr_image(fa, fw * n);
    S.z_addr = hr_image(za, fw * n);
    S.fb_width = fw * n;
    if (!depth) {                  /* colour goes 32-bit: no dithering, no banding */
        if (fs == 2) {
            u32 c = fill >> 16, r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
            S.fill = (r << 3 | r >> 2) << 24 | (g << 3 | g >> 2) << 16 | (b << 3 | b >> 2) << 8 | (c & 1 ? 0xE0u : 0);
        }
        S.fb_size = 3;
    }
    S.sc_field = 0;
    if (tri) { S.sc_xh *= n; S.sc_yh *= n; S.sc_xl *= n; S.sc_yl *= n; }
    hr_n = n; R_ram = hr_mem; R_hid = hr_hid; R_mask = HR_POOL - 1;
    exec_cmd(w);
    hr_n = 1; R_ram = n64_rdram; R_hid = hidden; R_mask = AMASK;
    S.fb_addr = fa; S.z_addr = za; S.fill = fill; S.rng = rng; S.fb_width = fw; S.fb_size = fs;
    S.sc_xh = xh; S.sc_yh = yh; S.sc_xl = xl; S.sc_yl = yl; S.sc_field = sf;
}

/* Copies the high-res image of `origin` into out (0x00RRGGBB), averaging each
 * aa x aa group of pixels. Returns 0 until that image has been drawn. */
int rdp_hires_fetch(u32 origin, int aa, u32 *out, int w, int h) {
    origin &= AMASK;
    for (int i = 0; i < HR_SLOTS; i++) {
        /* games point the video output a few lines into the image */
        const u32 skip = origin - hr_slot[i].addr;
        if (!hr_slot[i].stamp || skip > 0x2000) continue;
        const int lines = (int)skip / (hr_slot[i].width / hr_want * 2) * hr_want;
        const size_t stride = (size_t)hr_slot[i].width * 4;
        const u8 *base = hr_mem + (size_t)i * (HR_POOL / HR_SLOTS) + (size_t)lines * stride;
        for (int y = 0; y < h; y++) {
            const u8 *row = base + (size_t)y * aa * stride;
            for (int x = 0; x < w; x++, row += (size_t)aa * 4) {
                u32 r = 0, g = 0, b = 0;
                for (int v = 0; v < aa; v++) {
                    const u8 *p = row + (size_t)v * stride;
                    for (int u = 0; u < aa; u++, p += 4) { r += p[0]; g += p[1]; b += p[2]; }
                }
                const u32 k = (u32)(aa * aa);
                *out++ = (r / k) << 16 | (g / k) << 8 | (b / k);
            }
        }
        return 1;
    }
    return 0;
}

static void exec_cmd(const u64 *w) {
    const int op = (int)(w[0] >> 56) & 0x3F;
    if (rdp_debug && op != 0x27 && op != 0x28 && op != 0x29 && op != 0x26) fprintf(stderr, "RDP %02X %08X %08X\n", op, (u32)(w[0] >> 32), (u32)w[0]);
    const u32 hi = HI(w[0]), lo = LO(w[0]);
    if (op >= 0x08 && op <= 0x0F) {
        draw_tri(w, op & 4, op & 2, op & 1);
        if (hr_want > 1 && hr_n == 1) hr_replay(w, 1);
        return;
    }
    switch (op) {
    case 0x24: case 0x25: {
        u32 a = HI(w[1]), b = LO(w[1]);
        draw_rect((lo >> 12) & 0xFFF, lo & 0xFFF, (hi >> 12) & 0xFFF, hi & 0xFFF, (lo >> 24) & 7,
                  (s16)(a >> 16), (s16)a, (s16)(b >> 16), (s16)b, op & 1);
        if (hr_want > 1 && hr_n == 1) hr_replay(w, 0);
        break;
    }
    case 0x29: n64_rdp_full_sync(); break;
    case 0x2A:
        S.key_w[1] = (hi >> 12) & 0xFFF; S.key_w[2] = hi & 0xFFF;
        S.key_c[1] = (lo >> 24) & 0xFF; S.key_s[1] = (lo >> 16) & 0xFF;
        S.key_c[2] = (lo >> 8) & 0xFF; S.key_s[2] = lo & 0xFF;
        break;
    case 0x2B:
        S.key_w[0] = (lo >> 16) & 0xFFF; S.key_c[0] = (lo >> 8) & 0xFF; S.key_s[0] = lo & 0xFF;
        break;
    case 0x2C:
        S.k[0] = sx(hi >> 13, 9); S.k[1] = sx(hi >> 4, 9);
        S.k[2] = sx((hi << 5) | (lo >> 27), 9);
        S.k[3] = sx(lo >> 18, 9); S.k[4] = sx(lo >> 9, 9); S.k[5] = sx(lo, 9);
        break;
    case 0x2D:
        S.sc_xh = (hi >> 12) & 0xFFF; S.sc_yh = hi & 0xFFF;
        S.sc_xl = (lo >> 12) & 0xFFF; S.sc_yl = lo & 0xFFF;
        S.sc_field = (lo >> 25) & 1; S.sc_odd = (lo >> 24) & 1;
        break;
    case 0x2E: S.prim_z = (lo >> 16) & 0x7FFF; S.prim_dz = lo & 0xFFFF; break;
    case 0x2F: S.om = w[0]; break;
    case 0x30: load_tlut(hi, lo); break;
    case 0x32: tile_set_size(&S.tile[(lo >> 24) & 7], hi, lo); break;
    case 0x33: load_block(hi, lo); break;
    case 0x34: load_tile(hi, lo); break;
    case 0x35: {
        Tile *t = &S.tile[(lo >> 24) & 7];
        t->fmt = (hi >> 21) & 7; t->size = (hi >> 19) & 3;
        t->line = (hi >> 9) & 0x1FF; t->tmem = hi & 0x1FF;
        t->pal = (lo >> 20) & 15;
        t->ct = (lo >> 19) & 1; t->mt = (lo >> 18) & 1;
        t->mask_t = (lo >> 14) & 15; t->shift_t = (lo >> 10) & 15;
        t->cs = (lo >> 9) & 1; t->ms = (lo >> 8) & 1;
        t->mask_s = (lo >> 4) & 15; t->shift_s = lo & 15;
        tile_derive(t);
        break;
    }
    case 0x36:
        draw_rect((lo >> 12) & 0xFFF, lo & 0xFFF, (hi >> 12) & 0xFFF, hi & 0xFFF, 0, 0, 0, 0, 0, 0);
        if (hr_want > 1 && hr_n == 1) hr_replay(w, 0);
        break;
    case 0x37: S.fill = lo; break;
    case 0x38: S.fog = lo; break;
    case 0x39: S.blend = lo; break;
    case 0x3A: S.prim = lo; S.prim_lod_min = (hi >> 8) & 31; S.prim_lod_frac = hi & 0xFF; break;
    case 0x3B: S.env = lo; break;
    case 0x3C: S.combine = w[0] & 0x00FFFFFFFFFFFFFFull; break;
    case 0x3D:
        S.ti_fmt = (hi >> 21) & 7; S.ti_size = (hi >> 19) & 3;
        S.ti_width = (int)(hi & 0x3FF) + 1; S.ti_addr = lo & AMASK;
        break;
    case 0x3E: S.z_addr = lo & AMASK; break;
    case 0x3F:
        S.fb_fmt = (hi >> 21) & 7; S.fb_size = (hi >> 19) & 3;
        S.fb_width = (int)(hi & 0x3FF) + 1; S.fb_addr = lo & AMASK;
        break;
    default: break;     /* nop, Sync Pipe/Tile/Load, reserved */
    }
}

void rdp_run(u32 start, u32 end, int xbus) {
    u32 a = start & ~7u, e = end & ~7u;
    while (a < e) {
        const u8 *p = xbus ? &n64_sp_mem[a & 0xFF8] : &n64_rdram[a & AMASK & ~7u];
        u64 v;
        memcpy(&v, p, 8);
        a += 8;
        S.cmd[S.cmd_fill++] = __builtin_bswap64(v);
        int need = cmd_len[(S.cmd[0] >> 56) & 0x3F];
        if (need < 1) need = 1;
        if (S.cmd_fill >= need) {
            S.cmd_fill = 0;
            exec_cmd(S.cmd);
        }
    }
}

void rdp_reset(void) {
    memset(&S, 0, sizeof S);
    memset(hidden, 0, sizeof hidden);
    S.fb_size = 2;
    S.fb_width = 320;
    S.sc_xl = 320 << 2;
    S.sc_yl = 240 << 2;
    S.rng = 0x1234567;
    for (int i = 0; i < 8; i++) tile_derive(&S.tile[i]);
    M.valid = 0;
}

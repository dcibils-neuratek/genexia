/*
 * psp_ge.c - the PSP's graphics chip. The game hands it lists of 32-bit commands;
 * this walks them and draws into guest video memory with a software rasterizer,
 * so a game reading its own render targets sees what it would on the hardware.
 *
 * Not here yet: stencil, fog, dithering, colour test, mipmaps, beziers and splines.
 */
#include <stdio.h>
#include <stdlib.h>
#include "allegrex.h"
#ifdef PSP_METAL
#include "psp_metal.h"
#endif

static u32 ge[256];
static float world[12], view[12], proj[16], tgen[12], bone[8][12], morph[8] = { 1 };
static int world_i, view_i, proj_i, tgen_i, bone_i;
static u32 base, offset_addr, vaddr, iaddr;
static u8 clut[1024];
static float tex_su = 1, tex_sv = 1, tex_ou, tex_ov;

u64 psp_ge_tris, psp_ge_prims, psp_ge_unknown[256];

static float f24(u32 d) { u32 v = d << 8; float f; memcpy(&f, &v, 4); return f; }
static u32 full(u32 d) { return ((base | (d & 0xFFFFFF)) + offset_addr) & 0x0FFFFFFF; }

/* -------------------------------------------------------------- textures */
typedef struct { u32 key[4]; int w, h; u32 *px; u32 hash; void *gpu; } Tex;
#define TEX_SLOTS 1024
static Tex cache[TEX_SLOTS];
static const Tex *bound;
void psp_ge_new_frame(void) { for (int i = 0; i < TEX_SLOTS; i++) cache[i].key[0] = 0; bound = NULL; }

static u32 from5650(u32 c) { u32 r = c & 31, g = (c >> 5) & 63, b = (c >> 11) & 31; return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xFF000000u; }
static u32 from5551(u32 c) { u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31; return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | (c & 0x8000 ? 0xFF000000u : 0); }
static u32 from4444(u32 c) { u32 v = 0; for (int i = 0; i < 4; i++) { const u32 n = (c >> (i * 4)) & 15; v |= (n | n << 4) << (i * 8); } return v; }
static u32 convert(u32 c, int fmt) { return fmt == 0 ? from5650(c) : fmt == 1 ? from5551(c) : fmt == 2 ? from4444(c) : c; }
static u32 clut_entry(u32 i) {
    const u32 c = ge[0xC5]; const int fmt = c & 3, shift = (c >> 2) & 31; const u32 mask = (c >> 8) & 255, start = ((c >> 16) & 31) << 4;
    i = ((i >> shift) & mask) | start;
    if (fmt == 3) { u32 v; memcpy(&v, clut + (i & 255) * 4, 4); return v; }
    u16 v; memcpy(&v, clut + (i & 511) * 2, 2); return convert(v, fmt);
}
static void dxt_colors(const u8 *b, u32 *out, int alpha) {
    u16 c1, c2; memcpy(&c1, b + 4, 2); memcpy(&c2, b + 6, 2);
    const int r1 = (c1 >> 8) & 0xF8, g1 = (c1 >> 3) & 0xFC, b1 = (c1 << 3) & 0xF8, r2 = (c2 >> 8) & 0xF8, g2 = (c2 >> 3) & 0xFC, b2 = (c2 << 3) & 0xF8;
#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | 0xFF000000u)
    out[0] = RGB(r1, g1, b1); out[1] = RGB(r2, g2, b2);
    if (c1 > c2 || !alpha) {
        out[2] = RGB((r1 * 2 + r2) / 3, (g1 * 2 + g2) / 3, (b1 * 2 + b2) / 3);
        out[3] = RGB((r1 + r2 * 2) / 3, (g1 + g2 * 2) / 3, (b1 + b2 * 2) / 3);
    } else { out[2] = RGB((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2); out[3] = 0; }
}
static void decode_dxt(const u8 *src, u32 *out, int w, int h, int stride, int fmt) {
    const int size = fmt == 8 ? 8 : 16, across = (stride + 3) / 4;      /* rows follow the buffer, which can be narrower than the texture */
    for (int y = 0; y < h; y += 4) for (int x = 0; x < w && x / 4 < across; x += 4) {
        const u8 *s = src + (size_t)(y / 4) * across * size + (size_t)(x / 4) * size;
        u32 c[4], lines; dxt_colors(s, c, fmt == 8); memcpy(&lines, s, 4);
        u8 al[8] = { 0 }; u32 lo = 0; u16 hi = 0;
        if (fmt == 10) {
            memcpy(&lo, s + 8, 4); memcpy(&hi, s + 12, 2);
            const int a1 = s[14], a2 = s[15];
            al[0] = (u8)a1; al[1] = (u8)a2;
            if (a1 > a2) for (int i = 2; i < 8; i++) al[i] = (u8)(((8 - i) * a1 + (i - 1) * a2) / 7);
            else { for (int i = 2; i < 6; i++) al[i] = (u8)(((6 - i) * a1 + (i - 1) * a2) / 5); al[7] = 255; }
        }
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
            if (y + j >= h || x + i >= w) continue;
            u32 v = c[(lines >> ((j * 4 + i) * 2)) & 3];
            if (fmt == 9) { u16 a; memcpy(&a, s + 8 + j * 2, 2); const u32 n = (a >> (i * 4)) & 15; v = (v & 0xFFFFFF) | (n | n << 4) << 24; }
            if (fmt == 10) { const int bit = (j * 4 + i) * 3; const u64 bits = (u64)hi << 32 | lo; v = (v & 0xFFFFFF) | (u32)al[(bits >> bit) & 7] << 24; }
            out[(y + j) * w + x + i] = v;
        }
    }
}
static const Tex *decoded(Tex *t) {
#ifdef PSP_METAL
    u32 h = 2166136261u;
    for (int i = 0; i < t->w * t->h; i++) h = (h ^ t->px[i]) * 16777619u;
    if (h != t->hash || !t->gpu) { mtl_texture(&t->gpu, t->w, t->h, t->w, t->px); t->hash = h; }
#endif
    return t;
}
static const Tex *texture(void) {
    const u32 addr = (ge[0xA0] & 0xFFFFFF) | (ge[0xA8] & 0xFF0000) << 8, fmt = ge[0xC3] & 15, stride = ge[0xA8] & 0xFFFF;
    const int w = 1 << (ge[0xB8] & 15), h = 1 << ((ge[0xB8] >> 8) & 15);
    const u32 k1 = addr, k2 = fmt | stride << 8 | (u32)(ge[0xB8] & 0xFFFF) << 20, k3 = ge[0xC5], k4 = ge[0xC2] & 1;
    Tex *t = &cache[((addr >> 6) ^ (k3 * 31) ^ (k2 >> 20)) & (TEX_SLOTS - 1)];
    const int vram = (addr & 0x0F000000) == 0x04000000;
    if (t->key[0] == k1 && t->key[1] == k2 && t->key[2] == k3 && t->key[3] == k4 && !vram) return t;
    if (t->w < w || t->h < h || !t->px) { free(t->px); t->px = malloc((size_t)w * h * 4); }
    t->w = w; t->h = h; t->key[0] = k1; t->key[1] = k2; t->key[2] = k3; t->key[3] = k4;
    static const int bits[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
    const int bpp = fmt < 11 ? bits[fmt] : 32;
    const u8 *src = M(addr);
    if (fmt >= 8 && fmt <= 10) { decode_dxt(src, t->px, w, h, (int)stride, (int)fmt); return decoded(t); }
    static u8 *flat; static size_t cap;
    const size_t row = (size_t)((stride * bpp + 7) / 8), need = row * h + 4096;
    if (ge[0xC2] & 1) {                            /* swizzled: 16-by-8 byte blocks */
        if (need > cap) { free(flat); flat = malloc(need * 2); cap = need * 2; }
        const size_t rowb = row < 16 ? 16 : row; const int bx = (int)(rowb / 16), by = (h + 7) / 8;
        for (int y = 0; y < by; y++) for (int x = 0; x < bx; x++) for (int n = 0; n < 8; n++, src += 16)
            if ((y * 8 + n) * rowb + x * 16 + 16 <= need) memcpy(flat + (y * 8 + n) * rowb + x * 16, src, 16);
        src = flat;
    }
    for (int y = 0; y < h; y++) {
        const u8 *l = src + row * y; u32 *o = t->px + y * w;
        for (int x = 0; x < w; x++) {
            u16 h16; u32 v32;
            switch (fmt) {
            case 0: case 1: case 2: memcpy(&h16, l + x * 2, 2); o[x] = convert(h16, (int)fmt); break;
            case 3: memcpy(&o[x], l + x * 4, 4); break;
            case 4: o[x] = clut_entry((l[x / 2] >> (x & 1 ? 4 : 0)) & 15); break;
            case 5: o[x] = clut_entry(l[x]); break;
            case 6: memcpy(&h16, l + x * 2, 2); o[x] = clut_entry(h16); break;
            case 7: memcpy(&v32, l + x * 4, 4); o[x] = clut_entry(v32); break;
            default: o[x] = 0xFFFF00FFu; break;
            }
        }
    }
    return decoded(t);
}
static inline u32 texel(const Tex *t, int x, int y, int cu, int cv) {
    x = cu ? (x < 0 ? 0 : x >= t->w ? t->w - 1 : x) : x & (t->w - 1);
    y = cv ? (y < 0 ? 0 : y >= t->h ? t->h - 1 : y) : y & (t->h - 1);
    return t->px[y * t->w + x];
}
static inline u32 sample(const Tex *t, float u, float v) {
    const int cu = ge[0xC7] & 1, cv = (ge[0xC7] >> 8) & 1;
    if (!((ge[0xC6] >> 8) & 1)) return texel(t, (int)floorf(u * t->w), (int)floorf(v * t->h), cu, cv);
    const float fu = u * t->w - 0.5f, fv = v * t->h - 0.5f;
    const int x = (int)floorf(fu), y = (int)floorf(fv), a = (int)((fu - x) * 256), b = (int)((fv - y) * 256);
    const u32 c0 = texel(t, x, y, cu, cv), c1 = texel(t, x + 1, y, cu, cv), c2 = texel(t, x, y + 1, cu, cv), c3 = texel(t, x + 1, y + 1, cu, cv);
    u32 o = 0;
    for (int s = 0; s < 32; s += 8) {
        const int p0 = (c0 >> s) & 255, p1 = (c1 >> s) & 255, p2 = (c2 >> s) & 255, p3 = (c3 >> s) & 255;
        const int t0 = p0 * (256 - a) + p1 * a, t1 = p2 * (256 - a) + p3 * a;
        o |= (u32)((t0 * (256 - b) + t1 * b) >> 16) << s;
    }
    return o;
}

/* ---------------------------------------------------------------- pixels */
typedef struct { float x, y, z, w, u, v, r, g, b, a; } Vtx;

static u32 fb_addr, z_addr; static int fb_w, z_w, fb_fmt, clip_x1, clip_y1, clip_x2, clip_y2;
static void target(void) {
    fb_addr = 0x04000000u | (ge[0x9C] & 0x1FFFFF); fb_w = ge[0x9D] & 0x7FF;
    z_addr = 0x04000000u | (ge[0x9E] & 0x1FFFFF); z_w = ge[0x9F] & 0x7FF;
    fb_fmt = ge[0xD2] & 3;
    clip_x1 = ge[0xD4] & 0x3FF; clip_y1 = (ge[0xD4] >> 10) & 0x3FF;
    clip_x2 = ge[0xD5] & 0x3FF; clip_y2 = (ge[0xD5] >> 10) & 0x3FF;
    if (clip_x2 >= fb_w) clip_x2 = fb_w - 1;
    if (clip_x2 > 511) clip_x2 = 511;
    if (clip_y2 > 511) clip_y2 = 511;
}
static inline u32 fb_get(int x, int y) {
    if (fb_fmt == 3) { u32 v; memcpy(&v, M(fb_addr + (u32)(y * fb_w + x) * 4), 4); return v; }
    u16 v; memcpy(&v, M(fb_addr + (u32)(y * fb_w + x) * 2), 2); return convert(v, fb_fmt);
}
static inline void fb_put(int x, int y, u32 c) {
    if (fb_fmt == 3) { memcpy(M(fb_addr + (u32)(y * fb_w + x) * 4), &c, 4); return; }
    const u32 r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24; u16 v;
    if (fb_fmt == 0) v = (u16)(r >> 3 | (g >> 2) << 5 | (b >> 3) << 11);
    else if (fb_fmt == 1) v = (u16)(r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | (a > 127) << 15);
    else v = (u16)(r >> 4 | (g >> 4) << 4 | (b >> 4) << 8 | (a >> 4) << 12);
    memcpy(M(fb_addr + (u32)(y * fb_w + x) * 2), &v, 2);
}
static inline int compare(int func, int a, int b) {
    switch (func) { case 0: return 0; case 1: return 1; case 2: return a == b; case 3: return a != b; case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b; }
}
static inline int blend_factor(int f, int c, int sa, int da, int other, int fixed, int is_src) {
    switch (f) {
    case 0: return other; case 1: return 255 - other;
    case 2: return sa; case 3: return 255 - sa; case 4: return da; case 5: return 255 - da;
    case 6: return sa * 2 > 255 ? 255 : sa * 2; case 7: return sa * 2 > 255 ? 0 : 255 - sa * 2;
    case 8: return da * 2 > 255 ? 255 : da * 2; case 9: return da * 2 > 255 ? 0 : 255 - da * 2;
    default: return fixed;
    }
}
static inline void pixel(int x, int y, float z, int r, int g, int b, int a) {
    const u32 clear = ge[0xD3];
    if (clear & 1) {
        const u32 old = fb_get(x, y); u32 n = old;
        if (clear & 0x100) n = (n & 0xFF000000u) | (u32)r | (u32)g << 8 | (u32)b << 16;
        if (clear & 0x200) n = (n & 0xFFFFFF) | (u32)a << 24;
        fb_put(x, y, n);
        if (clear & 0x400) { const u16 d = (u16)(z < 0 ? 0 : z > 65535 ? 65535 : z); memcpy(M(z_addr + (u32)(y * z_w + x) * 2), &d, 2); }
        return;
    }
    if (ge[0x22] & 1) {
        const u32 t = ge[0xDB]; const int m = (t >> 16) & 255;
        if (!compare(t & 7, a & m, (t >> 8) & m & 255)) return;
    }
    const int depth = (int)(z < 0 ? 0 : z > 65535 ? 65535 : z);
    if (ge[0x23] & 1) {
        u16 d; memcpy(&d, M(z_addr + (u32)(y * z_w + x) * 2), 2);
        if (!compare(ge[0xDE] & 7, depth, d)) return;
    }
    u32 old = fb_get(x, y);
    if (ge[0x21] & 1) {
        const u32 m = ge[0xDF]; const int sf = m & 15, df = (m >> 4) & 15, eq = (m >> 8) & 7;
        int o[3]; const int s[3] = { r, g, b }, da = (int)(old >> 24);
        for (int i = 0; i < 3; i++) {
            const int d = (old >> (i * 8)) & 255, fa = (ge[0xE0] >> (i * 8)) & 255, fb = (ge[0xE1] >> (i * 8)) & 255;
            const int ka = blend_factor(sf, 0, a, da, d, fa, 1), kb = blend_factor(df, 0, a, da, s[i], fb, 0);
            int v;
            switch (eq) {
            case 0: v = (s[i] * ka + d * kb) / 255; break;
            case 1: v = (s[i] * ka - d * kb) / 255; break;
            case 2: v = (d * kb - s[i] * ka) / 255; break;
            case 3: v = s[i] < d ? s[i] : d; break;
            case 4: v = s[i] > d ? s[i] : d; break;
            default: v = abs(s[i] - d); break;
            }
            o[i] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
        r = o[0]; g = o[1]; b = o[2];
    }
    const u32 mask = (ge[0xE8] & 0xFFFFFF) | (ge[0xE9] & 255) << 24;
    fb_put(x, y, (((u32)r | (u32)g << 8 | (u32)b << 16 | (u32)a << 24) & ~mask) | (old & mask));
    if ((ge[0x23] & 1) && !(ge[0xE7] & 1)) { const u16 d = (u16)depth; memcpy(M(z_addr + (u32)(y * z_w + x) * 2), &d, 2); }
}
static inline void shade(int x, int y, float z, float u, float v, float r, float g, float b, float a, const Tex *t) {
    int cr = (int)r, cg = (int)g, cb = (int)b, ca = (int)a;
    if (t) {
        const u32 c = sample(t, u, v);
        const int tr = c & 255, tg = (c >> 8) & 255, tb = (c >> 16) & 255, ta = c >> 24;
        const u32 fn = ge[0xC9]; const int use_a = (fn >> 8) & 1;
        switch (fn & 7) {
        case 0: cr = cr * tr / 255; cg = cg * tg / 255; cb = cb * tb / 255; if (use_a) ca = ca * ta / 255; break;
        case 1: if (use_a) { cr = (cr * (255 - ta) + tr * ta) / 255; cg = (cg * (255 - ta) + tg * ta) / 255; cb = (cb * (255 - ta) + tb * ta) / 255; } else { cr = tr; cg = tg; cb = tb; } break;
        case 2: {
            const int er = ge[0xCA] & 255, eg = (ge[0xCA] >> 8) & 255, eb = (ge[0xCA] >> 16) & 255;
            cr = (cr * (255 - tr) + er * tr) / 255; cg = (cg * (255 - tg) + eg * tg) / 255; cb = (cb * (255 - tb) + eb * tb) / 255;
            if (use_a) ca = ca * ta / 255;
            break;
        }
        case 3: cr = tr; cg = tg; cb = tb; if (use_a) ca = ta; break;
        default: cr += tr; cg += tg; cb += tb; if (use_a) ca = ca * ta / 255; break;
        }
        if (fn & 0x10000) { cr *= 2; cg *= 2; cb *= 2; }
        if (cr > 255) cr = 255;
        if (cg > 255) cg = 255;
        if (cb > 255) cb = 255;
    }
    pixel(x, y, z, cr, cg, cb, ca);
}
#ifdef PSP_METAL
static MtlVert *out_v; static int out_n, out_cap;
static void emit(const Vtx *v, const Vtx *colour) {
    if (out_n == out_cap) { out_cap = out_cap ? out_cap * 2 : 4096; out_v = realloc(out_v, (size_t)out_cap * sizeof *out_v); }
    MtlVert *o = &out_v[out_n++];
    o->pos[0] = (v->x / 256 - 1) * v->w; o->pos[1] = (1 - v->y / 256) * v->w; o->pos[2] = v->z / 65535 * v->w; o->pos[3] = v->w;
    o->uv[0] = v->u; o->uv[1] = v->v;
    o->col[0] = colour->r / 255; o->col[1] = colour->g / 255; o->col[2] = colour->b / 255; o->col[3] = colour->a / 255;
}
static void gpu_flush(const Tex *t) {
    if (!out_n) return;
    MtlState s; memset(&s, 0, sizeof s);
    const int clear = ge[0xD3] & 1;
    s.uv_scale[0] = s.uv_scale[1] = 1;
    if (t) {
        const u32 addr = ((ge[0xA0] & 0xFFFFFF) | (ge[0xA8] & 0xFF0000) << 8) & 0x0FFFFFFF;
        if (mtl_is_target(addr)) { s.tex_target = addr; s.uv_scale[0] = t->w / 512.f; s.uv_scale[1] = t->h / 512.f; }
        else s.tex = t->gpu;
        s.clamp_u = ge[0xC7] & 1; s.clamp_v = (ge[0xC7] >> 8) & 1; s.linear = (ge[0xC6] >> 8) & 1;
        s.tfunc = ge[0xC9] & 7; s.use_alpha = (ge[0xC9] >> 8) & 1; s.doubled = (ge[0xC9] >> 16) & 1;
        for (int i = 0; i < 3; i++) s.env[i] = ((ge[0xCA] >> (i * 8)) & 255) / 255.f;
    }
    if (clear) {
        s.depth_write = (ge[0xD3] >> 10) & 1;
        s.write_mask = (ge[0xD3] & 0x100 ? 7 : 0) | (ge[0xD3] & 0x200 ? 8 : 0);
    } else {
        s.alpha_test = ge[0x22] & 1; s.alpha_func = ge[0xDB] & 7; s.alpha_ref = (ge[0xDB] >> 8) & 255; s.alpha_mask = (ge[0xDB] >> 16) & 255;
        s.blend = ge[0x21] & 1; s.src = ge[0xDF] & 15; s.dst = (ge[0xDF] >> 4) & 15; s.eq = (ge[0xDF] >> 8) & 7;
        for (int i = 0; i < 3; i++) { s.fix_a[i] = ((ge[0xE0] >> (i * 8)) & 255) / 255.f; s.fix_b[i] = ((ge[0xE1] >> (i * 8)) & 255) / 255.f; }
        s.depth_test = ge[0x23] & 1; s.depth_func = ge[0xDE] & 7; s.depth_write = s.depth_test && !(ge[0xE7] & 1);
        const u32 m = ge[0xE8];
        s.write_mask = ((m & 0xFF) != 0xFF ? 1 : 0) | ((m & 0xFF00) != 0xFF00 ? 2 : 0) | ((m & 0xFF0000) != 0xFF0000 ? 4 : 0) | (fb_fmt && (ge[0xE9] & 255) != 255 ? 8 : 0);
    }
    if (fb_fmt == 0) s.write_mask &= 7;
    s.sx1 = clip_x1; s.sy1 = clip_y1; s.sx2 = clip_x2; s.sy2 = clip_y2;
    mtl_draw(fb_addr, &s, out_v, out_n);
    out_n = 0;
}
#endif
static inline float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }
static void triangle(Vtx a, Vtx b, Vtx c, const Tex *t, int smooth) {
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0) return;
    if ((ge[0x1D] & 1) && !(ge[0xD3] & 1) && ((area > 0) == !(ge[0x9B] & 1))) return;
#ifdef PSP_METAL
    emit(&a, smooth ? &a : &c); emit(&b, smooth ? &b : &c); emit(&c, &c);
    return;
#endif
    if (area < 0) { Vtx s = b; b = c; c = s; area = -area; }
    float lo_x = fminf(a.x, fminf(b.x, c.x)), hi_x = fmaxf(a.x, fmaxf(b.x, c.x)), lo_y = fminf(a.y, fminf(b.y, c.y)), hi_y = fmaxf(a.y, fmaxf(b.y, c.y));
    if (hi_x < clip_x1 || lo_x > clip_x2 + 1 || hi_y < clip_y1 || lo_y > clip_y2 + 1) return;
    const int x0 = lo_x < clip_x1 ? clip_x1 : (int)lo_x, x1 = hi_x > clip_x2 ? clip_x2 : (int)hi_x;
    const int y0 = lo_y < clip_y1 ? clip_y1 : (int)lo_y, y1 = hi_y > clip_y2 ? clip_y2 : (int)hi_y;
    const float ia = 1 / a.w, ib = 1 / b.w, ic = 1 / c.w, inv = 1 / area;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const float px = x + 0.5f, py = y + 0.5f;
        const float l0 = edge(b.x, b.y, c.x, c.y, px, py), l1 = edge(c.x, c.y, a.x, a.y, px, py), l2 = edge(a.x, a.y, b.x, b.y, px, py);
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        const float w0 = l0 * inv, w1 = l1 * inv, w2 = l2 * inv;
        const float w = 1 / (w0 * ia + w1 * ib + w2 * ic), k0 = w0 * ia * w, k1 = w1 * ib * w, k2 = w2 * ic * w;
        const float z = w0 * a.z + w1 * b.z + w2 * c.z;
        if (smooth) shade(x, y, z, k0 * a.u + k1 * b.u + k2 * c.u, k0 * a.v + k1 * b.v + k2 * c.v,
                          k0 * a.r + k1 * b.r + k2 * c.r, k0 * a.g + k1 * b.g + k2 * c.g, k0 * a.b + k1 * b.b + k2 * c.b, k0 * a.a + k1 * b.a + k2 * c.a, t);
        else shade(x, y, z, k0 * a.u + k1 * b.u + k2 * c.u, k0 * a.v + k1 * b.v + k2 * c.v, c.r, c.g, c.b, c.a, t);
    }
}
static void rect(const Vtx *a, const Vtx *b, const Tex *t) {
#ifdef PSP_METAL
    Vtx p = *a, q = *a, r = *b, s = *b;
    p.w = q.w = r.w = s.w = 1; p.z = q.z = r.z = s.z = b->z;
    q.x = b->x; q.u = b->u; s.x = a->x; s.u = a->u;
    emit(&p, b); emit(&q, b); emit(&r, b); emit(&p, b); emit(&r, b); emit(&s, b);
    return;
#endif
    int x0 = (int)fminf(a->x, b->x), x1 = (int)fmaxf(a->x, b->x), y0 = (int)fminf(a->y, b->y), y1 = (int)fmaxf(a->y, b->y);
    const float dx = b->x - a->x, dy = b->y - a->y;
    if (x0 < clip_x1) x0 = clip_x1;
    if (y0 < clip_y1) y0 = clip_y1;
    if (x1 > clip_x2 + 1) x1 = clip_x2 + 1;
    if (y1 > clip_y2 + 1) y1 = clip_y2 + 1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        const float fx = dx ? (x + 0.5f - a->x) / dx : 0, fy = dy ? (y + 0.5f - a->y) / dy : 0;
        shade(x, y, b->z, a->u + (b->u - a->u) * fx, a->v + (b->v - a->v) * fy, b->r, b->g, b->b, b->a, t);
    }
}

/* -------------------------------------------------------------- vertices */
typedef struct { float pos[3], nrm[3], u, v, weight[8]; float col[4]; int has_col; } Raw;
static void layout(u32 vt, int *size, int *o_w, int *o_t, int *o_c, int *o_n, int *o_p) {
    static const int tcs[4] = { 0, 2, 4, 8 }, cols[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }, ps[4] = { 0, 3, 6, 12 }, ws[4] = { 0, 1, 2, 4 };
    static const int ta[4] = { 1, 1, 2, 4 }, ca[8] = { 1, 1, 1, 1, 2, 2, 2, 4 };
    const int tc = vt & 3, c = (vt >> 2) & 7, n = (vt >> 5) & 3, q = (vt >> 7) & 3, w = (vt >> 9) & 3, wn = ((vt >> 14) & 7) + 1;
    int o = 0, al = 1;
#define ALIGN(a) do { const int a_ = (a); o = (o + a_ - 1) & ~(a_ - 1); if (a_ > al) al = a_; } while (0)
    if (w) ALIGN(ws[w]);
    *o_w = o; if (w) o += ws[w] * wn;
    ALIGN(ta[tc]); *o_t = o; o += tcs[tc];
    ALIGN(ca[c]); *o_c = o; o += cols[c];
    ALIGN(ws[n] ? ws[n] : 1); *o_n = o; o += ps[n];
    ALIGN(ws[q] ? ws[q] : 1); *o_p = o; o += ps[q];
    ALIGN(al);
    *size = o;
}
static float comp(u32 a, int type, int unsign_scale, int through) {
    switch (type) {
    case 1: return unsign_scale ? (through ? RD8(a) : RD8(a) / 128.f) : (through ? (s8)RD8(a) : (s8)RD8(a) / 128.f);
    case 2: return unsign_scale ? (through ? RD16(a) : RD16(a) / 32768.f) : (through ? (s16)RD16(a) : (s16)RD16(a) / 32768.f);
    default: { u32 v = RD32(a); float f; memcpy(&f, &v, 4); return f; }
    }
}
static void read_vertex(u32 a, u32 vt, Raw *r) {
    int size, ow, ot, oc, on, op;
    layout(vt, &size, &ow, &ot, &oc, &on, &op);
    const int through = (vt >> 23) & 1, tc = vt & 3, c = (vt >> 2) & 7, n = (vt >> 5) & 3, q = (vt >> 7) & 3, w = (vt >> 9) & 3, wn = ((vt >> 14) & 7) + 1;
    static const int ws[4] = { 0, 1, 2, 4 };
    for (int i = 0; i < 8; i++) r->weight[i] = 0;
    if (w) for (int i = 0; i < wn; i++) r->weight[i] = comp(a + ow + i * ws[w], w, 1, 0);
    r->u = r->v = 0;
    if (tc) { r->u = comp(a + ot, tc, 1, through); r->v = comp(a + ot + ws[tc], tc, 1, through); }
    r->has_col = c != 0;
    if (c) {
        u32 v = 0;
        if (c == 7) v = RD32(a + oc); else v = convert(RD16(a + oc), c - 4);
        for (int i = 0; i < 4; i++) r->col[i] = (float)((v >> (i * 8)) & 255);
    }
    r->nrm[0] = r->nrm[1] = 0; r->nrm[2] = 1;
    if (n) for (int i = 0; i < 3; i++) r->nrm[i] = comp(a + on + i * ws[n], n, 0, 0);
    for (int i = 0; i < 3; i++) r->pos[i] = comp(a + op + i * ws[q], q, 0, through);
    if (through && q != 3) r->pos[2] = comp(a + op + 2 * ws[q], q, 1, 1);        /* depth is unsigned here */
}
static void mul43(const float *m, const float *v, float w, float *o) {
    float t[3];
    for (int i = 0; i < 3; i++) t[i] = m[i] * v[0] + m[3 + i] * v[1] + m[6 + i] * v[2] + m[9 + i] * w;
    memcpy(o, t, sizeof t);
}
static void normalize3(float *v) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void rgb(u32 c, float *o) { for (int i = 0; i < 3; i++) o[i] = (float)((c >> (i * 8)) & 255); }

static void light_vertex(const float *wp, const float *wn, const Raw *r, float *out) {
    float mat_a[3], mat_d[3], em[3], amb[3], res[3];
    const u32 upd = ge[0x53];
    rgb(ge[0x55], mat_a); rgb(ge[0x56], mat_d); rgb(ge[0x54], em); rgb(ge[0x5C], amb);
    float alpha = (float)(ge[0x58] & 255);
    if (r->has_col) {
        if (upd & 1) { memcpy(mat_a, r->col, 12); alpha = r->col[3]; }
        if (upd & 2) memcpy(mat_d, r->col, 12);
    }
    for (int i = 0; i < 3; i++) res[i] = em[i] + amb[i] * mat_a[i] / 255;
    for (int l = 0; l < 4; l++) {
        if (!(ge[0x18 + l] & 1)) continue;
        const u32 ty = ge[0x5F + l]; const int kind = (ty >> 8) & 3;
        float lp[3] = { f24(ge[0x63 + l * 3]), f24(ge[0x64 + l * 3]), f24(ge[0x65 + l * 3]) }, ld[3];
        float atten = 1;
        if (kind == 0) { memcpy(ld, lp, 12); normalize3(ld); }
        else {
            for (int i = 0; i < 3; i++) ld[i] = lp[i] - wp[i];
            const float d = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
            normalize3(ld);
            const float k = f24(ge[0x7B + l * 3]) + f24(ge[0x7C + l * 3]) * d + f24(ge[0x7D + l * 3]) * d * d;
            atten = k > 0 ? 1 / k : 1;
            if (atten > 1) atten = 1;
        }
        float dot = ld[0] * wn[0] + ld[1] * wn[1] + ld[2] * wn[2];
        if (dot < 0) dot = 0;
        float la[3], lc[3];
        rgb(ge[0x8F + l * 3], la); rgb(ge[0x90 + l * 3], lc);
        for (int i = 0; i < 3; i++) res[i] += (la[i] * mat_a[i] / 255 + lc[i] * mat_d[i] / 255 * dot) * atten;
    }
    for (int i = 0; i < 3; i++) out[i] = res[i] > 255 ? 255 : res[i];
    out[3] = alpha + (float)(ge[0x5D] & 0) ;
}
static Vtx transform(const Raw *r, u32 vt, const Tex *t) {
    Vtx o; const int through = (vt >> 23) & 1;
    if (r->has_col) { o.r = r->col[0]; o.g = r->col[1]; o.b = r->col[2]; o.a = r->col[3]; }
    else { o.r = (float)(ge[0x55] & 255); o.g = (float)((ge[0x55] >> 8) & 255); o.b = (float)((ge[0x55] >> 16) & 255); o.a = (float)(ge[0x58] & 255); }
    if (through) {
        o.x = r->pos[0]; o.y = r->pos[1]; o.z = r->pos[2]; o.w = 1;
        o.u = t ? r->u / t->w : r->u; o.v = t ? r->v / t->h : r->v;
        return o;
    }
    float pos[3], nrm[3], wp[3], wn[3], vp[3];
    memcpy(pos, r->pos, 12); memcpy(nrm, r->nrm, 12);
    if ((vt >> 9) & 3) {
        const int wn_c = ((vt >> 14) & 7) + 1;
        float sp[3] = { 0 }, sn[3] = { 0 };
        for (int i = 0; i < wn_c; i++) {
            float a[3], b[3];
            mul43(bone[i], r->pos, 1, a); mul43(bone[i], r->nrm, 0, b);
            for (int k = 0; k < 3; k++) { sp[k] += a[k] * r->weight[i]; sn[k] += b[k] * r->weight[i]; }
        }
        memcpy(pos, sp, 12); memcpy(nrm, sn, 12);
    }
    mul43(world, pos, 1, wp); mul43(world, nrm, 0, wn);
    normalize3(wn);
    if (ge[0x51] & 1) for (int i = 0; i < 3; i++) wn[i] = -wn[i];
    mul43(view, wp, 1, vp);
    o.x = proj[0] * vp[0] + proj[4] * vp[1] + proj[8] * vp[2] + proj[12];
    o.y = proj[1] * vp[0] + proj[5] * vp[1] + proj[9] * vp[2] + proj[13];
    o.z = proj[2] * vp[0] + proj[6] * vp[1] + proj[10] * vp[2] + proj[14];
    o.w = proj[3] * vp[0] + proj[7] * vp[1] + proj[11] * vp[2] + proj[15];
    if (ge[0x17] & 1) { float c[4]; light_vertex(wp, wn, r, c); o.r = c[0]; o.g = c[1]; o.b = c[2]; o.a = c[3]; }
    const int mode = ge[0xC0] & 3;
    if (mode == 0) { o.u = r->u * tex_su + tex_ou; o.v = r->v * tex_sv + tex_ov; }
    else if (mode == 1) {
        const int src = (ge[0xC0] >> 8) & 3;
        float in[3] = { r->u, r->v, 0 }, tv[3];
        if (src == 0) memcpy(in, r->pos, 12);
        else if (src == 2 || src == 3) { memcpy(in, r->nrm, 12); if (src == 2) normalize3(in); }
        mul43(tgen, in, 1, tv);
        const float q = tv[2] != 0 ? tv[2] : 1;
        o.u = tv[0] / q; o.v = tv[1] / q;          /* the matrix has the scale in it already */
    } else {                                      /* environment map from two lights */
        const int ls = ge[0xC1] & 3, lt = (ge[0xC1] >> 8) & 3;
        float a[3] = { f24(ge[0x63 + ls * 3]), f24(ge[0x64 + ls * 3]), f24(ge[0x65 + ls * 3]) }, b[3] = { f24(ge[0x63 + lt * 3]), f24(ge[0x64 + lt * 3]), f24(ge[0x65 + lt * 3]) };
        normalize3(a); normalize3(b);
        o.u = (1 + a[0] * wn[0] + a[1] * wn[1] + a[2] * wn[2]) / 2 * tex_su + tex_ou;
        o.v = (1 + b[0] * wn[0] + b[1] * wn[1] + b[2] * wn[2]) / 2 * tex_sv + tex_ov;
    }
    return o;
}
static Vtx to_screen(Vtx v) {
    const float iw = 1 / v.w;
    v.x = f24(ge[0x42]) * v.x * iw + f24(ge[0x45]) - (ge[0x4C] & 0xFFFF) / 16.f;
    v.y = f24(ge[0x43]) * v.y * iw + f24(ge[0x46]) - (ge[0x4D] & 0xFFFF) / 16.f;
    v.z = f24(ge[0x44]) * v.z * iw + f24(ge[0x47]);
    return v;
}
static Vtx lerp(const Vtx *a, const Vtx *b, float t) {
    Vtx o; const float *x = &a->x, *y = &b->x; float *z = &o.x;
    for (int i = 0; i < 10; i++) z[i] = x[i] + (y[i] - x[i]) * t;
    return o;
}
/* clip against the near plane (z + w >= 0, w a little above 0), draw what's left */
static void draw_tri(Vtx a, Vtx b, Vtx c, const Tex *tex, int through, int smooth) {
    psp_ge_tris++;
    if (through) { triangle(a, b, c, tex, smooth); return; }
    Vtx in[3] = { a, b, c }, out[4]; int n = 0;
    for (int i = 0; i < 3; i++) {
        const Vtx *p0 = &in[i], *p1 = &in[(i + 1) % 3];
        const float d0 = p0->z + p0->w, d1 = p1->z + p1->w;
        if (d0 >= 0) out[n++] = *p0;
        if ((d0 < 0) != (d1 < 0)) out[n++] = lerp(p0, p1, d0 / (d0 - d1));
    }
    if (n < 3) return;
    for (int i = 0; i < n; i++) if (out[i].w <= 1e-5f) return;
    for (int i = 0; i < n; i++) out[i] = to_screen(out[i]);
    triangle(out[0], out[1], out[2], tex, smooth);
    if (n == 4) triangle(out[0], out[2], out[3], tex, smooth);
}
static void prim(u32 d) {
    const u32 vt = ge[0x12]; const int count = d & 0xFFFF, type = (d >> 16) & 7, through = (vt >> 23) & 1, idx = (vt >> 11) & 3;
    const int morphs = ((vt >> 18) & 7) + 1, smooth = ge[0x50] & 1;
    int size, a, b, c, e, f;
    layout(vt, &size, &a, &b, &c, &e, &f);
    target();
    psp_ge_prims++;
    const Tex *t = (ge[0x1E] & 1) && !(ge[0xD3] & 1) ? texture() : NULL;
    Vtx prev[3]; int have = 0;
    Vtx rect_a;
    for (int i = 0; i < count; i++) {
        int n = i;
        if (idx == 1) n = RD8(iaddr + (u32)i); else if (idx == 2) n = RD16(iaddr + (u32)i * 2);
        Raw r;
        const u32 at = vaddr + (u32)(n * size * morphs);
        read_vertex(at, vt, &r);
        if (morphs > 1) {
            Raw s; float m[3] = { 0 }, q[3] = { 0 };
            for (int k = 0; k < morphs; k++) {
                read_vertex(at + (u32)(k * size), vt, &s);
                for (int j = 0; j < 3; j++) { m[j] += s.pos[j] * morph[k]; q[j] += s.nrm[j] * morph[k]; }
            }
            memcpy(r.pos, m, 12); memcpy(r.nrm, q, 12);
        }
        Vtx v = transform(&r, vt, t);
        switch (type) {
        case 3: prev[have++] = v; if (have == 3) { draw_tri(prev[0], prev[1], prev[2], t, through, smooth); have = 0; } break;
        case 4:
            if (have < 2) prev[have++] = v;
            else { if (i & 1) draw_tri(prev[1], prev[0], v, t, through, smooth); else draw_tri(prev[0], prev[1], v, t, through, smooth); prev[0] = prev[1]; prev[1] = v; }
            break;
        case 5:
            if (have < 2) prev[have++] = v;
            else { draw_tri(prev[0], prev[1], v, t, through, smooth); prev[1] = v; }
            break;
        case 6:
            if (!through) { if (v.w <= 0) { have = 0; break; } v = to_screen(v); }
            if (have == 0) { rect_a = v; have = 1; } else { rect(&rect_a, &v, t); have = 0; }
            break;
        default: break;                            /* points and lines */
        }
    }
#ifdef PSP_METAL
    gpu_flush(t);
#endif
    if (idx) iaddr += (u32)count * (idx == 1 ? 1 : 2);
    else vaddr += (u32)(count * size * morphs);
}

static void transfer(void) {
    const int bpp = ge[0xEA] & 1 ? 4 : 2, w = (ge[0xEE] & 0x3FF) + 1, h = ((ge[0xEE] >> 10) & 0x3FF) + 1;
    const u32 src = (ge[0xB2] & 0xFFFFFF) | (ge[0xB3] & 0xFF0000) << 8, dst = (ge[0xB4] & 0xFFFFFF) | (ge[0xB5] & 0xFF0000) << 8;
    const int sw = ge[0xB3] & 0xFFFF, dw = ge[0xB5] & 0xFFFF;
    const int sx = ge[0xEB] & 0x3FF, sy = (ge[0xEB] >> 10) & 0x3FF, dx = ge[0xEC] & 0x3FF, dy = (ge[0xEC] >> 10) & 0x3FF;
    for (int y = 0; y < h; y++) memmove(M(dst + (u32)(((dy + y) * dw + dx) * bpp)), M(src + (u32)(((sy + y) * sw + sx) * bpp)), (size_t)(w * bpp));
}

/* --------------------------------------------------------- walking lists */
extern int psp_ge_finishes, psp_ge_signals;
typedef struct { int used, finished; u32 pc, stall, id; u32 stack[32][2]; int sp; } List;
static List lists[16];
static u32 next_id = 1;
static int order[16], queued;

static void step_list(List *l) {
    int after_signal = 0;
    for (int guard = 0; guard < 50000000; guard++) {
        if (l->stall && l->pc == l->stall) return;
        const u32 w = RD32(l->pc), cmd = w >> 24, d = w & 0xFFFFFF;
        const u32 at = l->pc;
        l->pc += 4;
        ge[cmd] = d;
        switch (cmd) {
        case 0x00: break;
        case 0x01: vaddr = full(d); break;
        case 0x02: iaddr = full(d); break;
        case 0x04: prim(d); break;
        case 0x05: case 0x06: psp_ge_unknown[cmd]++; break;
        case 0x08: l->pc = full(d & ~3u); break;
        case 0x09: break;
        case 0x0A: if (l->sp < 32) { l->stack[l->sp][0] = l->pc; l->stack[l->sp][1] = offset_addr; l->sp++; } l->pc = full(d & ~3u); break;
        case 0x0B: if (l->sp > 0) { l->sp--; l->pc = l->stack[l->sp][0]; offset_addr = l->stack[l->sp][1]; } break;
        case 0x0C: if (after_signal) { after_signal = 0; break; } l->finished = 1; psp_ge_finishes++; return;
        case 0x0E: after_signal = 1; psp_ge_signals++; break;
        case 0x0F: break;
        case 0x10: base = (d & 0xFF0000) << 8; break;
        case 0x13: offset_addr = d << 8; break;
        case 0x14: offset_addr = at; break;
        case 0x2A: bone_i = (int)d & 127; break;
        case 0x2B: if (bone_i < 96) bone[bone_i / 12][bone_i % 12] = f24(d); bone_i++; break;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: morph[cmd - 0x2C] = f24(d); break;
        case 0x3A: world_i = d & 15; break;
        case 0x3B: if (world_i < 12) world[world_i++] = f24(d); break;
        case 0x3C: view_i = d & 15; break;
        case 0x3D: if (view_i < 12) view[view_i++] = f24(d); break;
        case 0x3E: proj_i = d & 15; break;
        case 0x3F: if (proj_i < 16) proj[proj_i++] = f24(d); break;
        case 0x40: tgen_i = d & 15; break;
        case 0x41: if (tgen_i < 12) tgen[tgen_i++] = f24(d); break;
        case 0x48: tex_su = f24(d); break;
        case 0x49: tex_sv = f24(d); break;
        case 0x4A: tex_ou = f24(d); break;
        case 0x4B: tex_ov = f24(d); break;
        case 0xC4: { const u32 a = (ge[0xB0] & 0xFFFFFF) | (ge[0xB1] & 0xFF0000) << 8, n = (d & 63) * 32; memcpy(clut, M(a), n > sizeof clut ? sizeof clut : n); break; }
        case 0xEA: transfer(); break;
        default: break;
        }
    }
}
static void run_queue(void) {
    while (queued) {
        List *l = &lists[order[0]];
        step_list(l);
        if (!l->finished) return;
        l->used = 0;
        for (int i = 1; i < queued; i++) order[i - 1] = order[i];
        queued--;
    }
}
u32 psp_ge_enqueue(u32 list, u32 stall) {
    for (int i = 0; i < 16; i++) if (!lists[i].used) {
        List *l = &lists[i];
        memset(l, 0, sizeof *l);
        l->used = 1; l->pc = list & 0x0FFFFFFF; l->stall = stall & 0x0FFFFFFF; l->id = next_id++;
        order[queued++] = i;
        run_queue();
        return l->id;
    }
    return 0x80000022u;
}
void psp_ge_update_stall(u32 id, u32 stall) {
    for (int i = 0; i < 16; i++) if (lists[i].used && lists[i].id == id) lists[i].stall = stall & 0x0FFFFFFF;
    run_queue();
}

void psp_ge_rgba(u32 addr, int stride, int fmt, u32 *out) {
    for (int y = 0; y < 272; y++) for (int x = 0; x < 480; x++) {
        u32 c;
        if (fmt == 3) memcpy(&c, M(addr + (u32)(y * stride + x) * 4), 4);
        else { u16 h; memcpy(&h, M(addr + (u32)(y * stride + x) * 2), 2); c = convert(h, fmt); }
        *out++ = c | 0xFF000000u;
    }
}
void psp_ge_shot(const char *path, u32 addr, int stride, int fmt) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n480 272\n255\n");
    static u8 row[480 * 3];
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < 480; x++) {
            u32 c;
            if (fmt == 3) memcpy(&c, M(addr + (u32)(y * stride + x) * 4), 4);
            else { u16 h; memcpy(&h, M(addr + (u32)(y * stride + x) * 2), 2); c = convert(h, fmt); }
            row[x * 3] = (u8)c; row[x * 3 + 1] = (u8)(c >> 8); row[x * 3 + 2] = (u8)(c >> 16);
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
}

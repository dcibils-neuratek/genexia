/*
 * f3d.c - reads Fast3D display lists directly, standing in for the RSP's
 * graphics microcode and the RDP, and draws them with OpenGL 1.2 plus the
 * extensions on the iMac's GeForce2 MX: two texture units, env_combine and
 * edge clamp. Vertices are transformed and lit here; the card clips and fills.
 *
 * The console's framebuffer stays untouched: only Super Mario 64 goes through
 * this path and it never reads pictures back.
 */
#include <string.h>
#include <stdio.h>
#include "f3d.h"
#include "n64.h"

#define R32(p) ((u32)(p)[0] << 24 | (u32)(p)[1] << 16 | (u32)(p)[2] << 8 | (p)[3])
#define R16(p) ((u16)((p)[0] << 8 | (p)[1]))
#define BATCH 3000

typedef struct { float x, y, z, w; short s, t; u8 r, g, b, a; u8 cc; } Vtx;
typedef struct { u8 fmt, siz, pal, cms, cmt, masks, maskt, shifts, shiftt; u16 line, tmem, uls, ult, lrs, lrt; } Tile;

static u32 seg[16];
static float mv[10][16], proj[16], mvp[16];
static int mv_top, mvp_ok, lights_ok;
static u32 geom, oth_l, oth_h, cc_hi, cc_lo;
static u8 c_prim[4], c_env[4], c_fog[4], c_blend[4];
static u32 c_fill;
static int tex_on, tex_tile;
static u32 tex_ss, tex_st;
static float vp_sx, vp_sy, vp_tx, vp_ty;
static int n_lights;
static float l_col[9][3];
static s8 l_raw[8][3];
static float l_dir[8][3], look_x[3], look_y[3];
static s8 look_raw[2][3];
static float fog_mul, fog_off;
static Vtx v[32];
static u32 timg_addr, zimg_addr, cimg_addr;
static int timg_siz, timg_w;
static Tile tiles[8];
static u8 tmem[4096];
static u16 tlut[256];
static u32 tmem_gen;
static int sc_x0, sc_y0, sc_x1, sc_y1;

static float b_pos[BATCH * 3][4], b_st[BATCH * 3][2];
static u8 b_col[BATCH * 3][4];
static int b_n;
static int dirty = 1;

f3d_stats_t f3d_stats;

static inline u32 addr(u32 a) { return (seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0x7FFFFF; }

/* ---------------------------------------------------------------- matrices */
static void mul44(float *o, const float *a, const float *b) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[c + 4] + a[r * 4 + 2] * b[c + 8] + a[r * 4 + 3] * b[c + 12];
    memcpy(o, t, sizeof t);
}

static void load_mtx(float *m, u32 pa) {
    const u8 *p = &n64_rdram[pa];
    for (int i = 0; i < 16; i++) {
        const s32 x = (s32)((u32)R16(p + i * 2) << 16 | R16(p + 32 + i * 2));
        m[i] = (float)x / 65536.0f;
    }
}

static void g_mtx(u32 w0, u32 w1) {
    const int flags = (w0 >> 16) & 0xFF;
    float m[16];
    load_mtx(m, addr(w1));
    if (flags & 1) {
        if (flags & 2) memcpy(proj, m, sizeof m);
        else mul44(proj, m, proj);
    } else {
        if ((flags & 4) && mv_top < 9) { memcpy(mv[mv_top + 1], mv[mv_top], sizeof mv[0]); mv_top++; }
        if (flags & 2) memcpy(mv[mv_top], m, sizeof m);
        else mul44(mv[mv_top], m, mv[mv_top]);
        lights_ok = 0;
    }
    mvp_ok = 0;
}

/* ---------------------------------------------------------------- vertices */
static void norm3(float *o, const float *i) {
    float d = i[0] * i[0] + i[1] * i[1] + i[2] * i[2];
    if (d > 0) {
        /* the same estimate-free inverse root on both hosts: accuracy here only
         * matters to the eye, and light directions change rarely */
        float x = d > 1 ? d : 1;
        for (int k = 0; k < 12; k++) x = 0.5f * (x + d / x);
        x = 1.0f / x;
        o[0] = i[0] * x; o[1] = i[1] * x; o[2] = i[2] * x;
    } else o[0] = o[1] = o[2] = 0;
}

/* The microcode rotates light directions into model space; so does this. */
static void model_dir(float *o, const s8 *raw) {
    const float *m = mv[mv_top];
    float t[3];
    t[0] = m[0] * raw[0] + m[1] * raw[1] + m[2] * raw[2];
    t[1] = m[4] * raw[0] + m[5] * raw[1] + m[6] * raw[2];
    t[2] = m[8] * raw[0] + m[9] * raw[1] + m[10] * raw[2];
    norm3(o, t);
}

static void update_lights(void) {
    lights_ok = 1;
    for (int i = 0; i < n_lights; i++) model_dir(l_dir[i], l_raw[i]);
    model_dir(look_x, look_raw[0]);
    model_dir(look_y, look_raw[1]);
}

static void g_vtx(u32 w0, u32 w1) {
    const int n = ((w0 >> 20) & 15) + 1, first = (w0 >> 16) & 15;
    const u8 *p = &n64_rdram[addr(w1)];
    if (!mvp_ok) { mul44(mvp, mv[mv_top], proj); mvp_ok = 1; }
    if ((geom & 0x60000) && !lights_ok) update_lights();
    const float *m = mvp;
    for (int i = 0; i < n && first + i < 16; i++, p += 16) {
        Vtx *o = &v[first + i];
        const float x = (s16)R16(p), y = (s16)R16(p + 2), z = (s16)R16(p + 4);
        const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float cz = x * m[2] + y * m[6] + z * m[10] + m[14];
        const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        /* the viewport, in terms of a 320x240 picture */
        o->x = cx * vp_sx + cw * vp_tx;
        o->y = cy * vp_sy + cw * vp_ty;
        o->z = cz; o->w = cw;
        o->cc = (cx < -cw) | (cx > cw) << 1 | (cy < -cw) << 2 | (cy > cw) << 3 | (cz > cw) << 4 | (cw <= 0) << 5;
        o->s = (s16)R16(p + 8); o->t = (s16)R16(p + 10);
        o->a = p[15];
        if (geom & 0x20000) {
            const float nx = (s8)p[12], ny = (s8)p[13], nz = (s8)p[14];
            float r = l_col[n_lights][0], g = l_col[n_lights][1], b = l_col[n_lights][2];
            for (int l = 0; l < n_lights; l++) {
                const float d = (nx * l_dir[l][0] + ny * l_dir[l][1] + nz * l_dir[l][2]) / 127.0f;
                if (d > 0) { r += l_col[l][0] * d; g += l_col[l][1] * d; b += l_col[l][2] * d; }
            }
            o->r = r > 255 ? 255 : (u8)r; o->g = g > 255 ? 255 : (u8)g; o->b = b > 255 ? 255 : (u8)b;
            if (geom & 0x40000) {          /* reflections, the metal cap */
                const float dx = (nx * look_x[0] + ny * look_x[1] + nz * look_x[2]) / 127.0f;
                const float dy = (nx * look_y[0] + ny * look_y[1] + nz * look_y[2]) / 127.0f;
                o->s = (short)((dx + 1) * 512 * 32 / 32 * 16);
                o->t = (short)((dy + 1) * 512 * 32 / 32 * 16);
            }
        } else { o->r = p[12]; o->g = p[13]; o->b = p[14]; }
        if (geom & 0x10000) {              /* fog goes in the alpha */
            const float f = (cw > 0 ? cz / cw : -1) * fog_mul + fog_off;
            o->a = f < 0 ? 0 : f > 255 ? 255 : (u8)f;
        }
    }
}

/* ---------------------------------------------------------------- textures */
#define TEX_SLOTS 1024
typedef struct { u32 key, name; float w, h; } TexSlot;
static TexSlot tex_slot[TEX_SLOTS];
static u8 tex_rgba[1024 * 1024 * 4 / 16];
static u32 tex_white;

static inline int pot(int n) { int p = 1; while (p < n && p < 512) p <<= 1; return p; }
static inline u8 x5(u32 v) { return (u8)(v << 3 | v >> 2); }

static void texel(const Tile *t, int x, int y, u8 *o) {
    const int line = t->line * 8, base = t->tmem * 8;
    switch (t->fmt << 4 | t->siz) {
    case 0x02: {                                    /* RGBA16 */
        const u8 *p = &tmem[(base + y * line + x * 2) & 0xFFE];
        const u32 c = R16(p);
        o[0] = x5(c >> 11 & 31); o[1] = x5(c >> 6 & 31); o[2] = x5(c >> 1 & 31); o[3] = c & 1 ? 255 : 0;
        return;
    }
    case 0x03: {                                    /* RGBA32: split between the banks */
        const int a = (base + y * line + x * 2) & 0x7FE;
        o[0] = tmem[a]; o[1] = tmem[a + 1]; o[2] = tmem[a + 0x800]; o[3] = tmem[a + 0x801];
        return;
    }
    case 0x20: case 0x21: {                         /* CI4, CI8 */
        const u8 b = tmem[(base + y * line + (t->siz ? x : x >> 1)) & 0xFFF];
        const int i = t->siz ? b : (t->pal << 4 | (x & 1 ? b & 15 : b >> 4));
        const u32 c = tlut[i];
        if (oth_h >> 14 & 3) {
            if ((oth_h >> 14 & 3) == 3) { o[0] = o[1] = o[2] = (u8)(c >> 8); o[3] = (u8)c; return; }
            o[0] = x5(c >> 11 & 31); o[1] = x5(c >> 6 & 31); o[2] = x5(c >> 1 & 31); o[3] = c & 1 ? 255 : 0;
        } else o[0] = o[1] = o[2] = o[3] = (u8)i;
        return;
    }
    case 0x30: {                                    /* IA4 */
        const u8 b = tmem[(base + y * line + (x >> 1)) & 0xFFF];
        const u32 n = x & 1 ? b & 15 : b >> 4, i = n >> 1;
        o[0] = o[1] = o[2] = (u8)(i << 5 | i << 2 | i >> 1); o[3] = n & 1 ? 255 : 0;
        return;
    }
    case 0x31: {                                    /* IA8 */
        const u8 b = tmem[(base + y * line + x) & 0xFFF];
        o[0] = o[1] = o[2] = (u8)((b >> 4) * 17); o[3] = (u8)((b & 15) * 17);
        return;
    }
    case 0x32: {                                    /* IA16 */
        const u8 *p = &tmem[(base + y * line + x * 2) & 0xFFE];
        o[0] = o[1] = o[2] = p[0]; o[3] = p[1];
        return;
    }
    case 0x40: {                                    /* I4 */
        const u8 b = tmem[(base + y * line + (x >> 1)) & 0xFFF];
        o[0] = o[1] = o[2] = o[3] = (u8)((x & 1 ? b & 15 : b >> 4) * 17);
        return;
    }
    case 0x41:                                      /* I8 */
        o[0] = o[1] = o[2] = o[3] = tmem[(base + y * line + x) & 0xFFF];
        return;
    }
    o[0] = o[1] = o[2] = 255; o[3] = 255;
}

static u32 tile_w, tile_h;

static void tile_size(const Tile *t, int *w, int *h) {
    int sw = ((t->lrs - t->uls) >> 2) + 1, sh = ((t->lrt - t->ult) >> 2) + 1;
    if (t->masks && !(t->cms & 2)) sw = 1 << t->masks;
    if (t->maskt && !(t->cmt & 2)) sh = 1 << t->maskt;
    if (sw > 256) sw = 256;
    if (sh > 256) sh = 256;
    *w = sw; *h = sh;
}

static u32 tile_hash(const Tile *t, int w, int h) {
    u32 k = 2166136261u;
    const int bytes = (h * t->line * 8) ? h * t->line * 8 : 4096;
    const int from = t->tmem * 8, n = bytes > 4096 - from ? 4096 - from : bytes;
    for (int i = 0; i + 4 <= n; i += 4) k = (k ^ R32(&tmem[from + i])) * 16777619u;
    if (t->fmt == 2) for (int i = 0; i < 256; i += 3) k = (k ^ tlut[i]) * 16777619u;
    const u32 d = (u32)t->fmt | t->siz << 3 | t->pal << 5 | t->cms << 9 | t->cmt << 11 | w << 13 | (u32)h << 22;
    return (k ^ d ^ (oth_h >> 14 & 3) * 77u) * 16777619u | 1;
}

static const Tile *bound_tile;
static float bind_tile(const Tile *t, float *sc_t) {
    int w, h;
    tile_size(t, &w, &h);
    const u32 k = tile_hash(t, w, h);
    TexSlot *s = &tex_slot[k % TEX_SLOTS];
    if (s->key != k) {
        const int ms = (t->cms & 1) != 0, mt = (t->cmt & 1) != 0;   /* no mirrored repeat in 1.2 */
        const int tw = pot(w << ms), th = pot(h << mt);
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                int sx = x, sy = y;
                if (ms) { sx %= w * 2; if (sx >= w) sx = 2 * w - 1 - sx; }
                else if (sx >= w) sx = w - 1;
                if (mt) { sy %= h * 2; if (sy >= h) sy = 2 * h - 1 - sy; }
                else if (sy >= h) sy = h - 1;
                texel(t, sx, sy, &tex_rgba[(y * tw + x) * 4]);
            }
        }
        if (!s->name) glGenTextures(1, (GLuint *)&s->name);
        s->key = k; s->w = (float)tw; s->h = (float)th;
        glBindTexture(GL_TEXTURE_2D, s->name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_rgba);
        f3d_stats.tex_uploads++;
    } else glBindTexture(GL_TEXTURE_2D, s->name);
    /* wrap only when the whole texture repeats; a clamped one padded to a power
     * of two carries its edge colour on */
    const int rs = !(t->cms & 2) && t->masks, rt = !(t->cmt & 2) && t->maskt;
    const int f = (oth_h >> 12 & 3) ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, rs ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, rt ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    *sc_t = s->h;
    return s->w;
}

/* ---------------------------------------------------------------- combiner */
enum { S_0, S_1, S_T, S_TA, S_SHADE, S_SHADEA, S_PRIM, S_PRIMA, S_ENV, S_ENVA, S_CMB };
static const u8 rgb_a[16] = { S_CMB, S_T, S_T, S_PRIM, S_SHADE, S_ENV, S_1, S_0, S_0, S_0, S_0, S_0, S_0, S_0, S_0, S_0 };
static const u8 rgb_b[16] = { S_CMB, S_T, S_T, S_PRIM, S_SHADE, S_ENV, S_0, S_0, S_0, S_0, S_0, S_0, S_0, S_0, S_0, S_0 };
static const u8 rgb_c[32] = { S_CMB, S_T, S_T, S_PRIM, S_SHADE, S_ENV, S_0, S_0, S_TA, S_TA, S_PRIMA, S_SHADEA, S_ENVA, S_0, S_0, S_0 };
static const u8 rgb_d[8] = { S_CMB, S_T, S_T, S_PRIM, S_SHADE, S_ENV, S_1, S_0 };
static const u8 al_abd[8] = { S_CMB, S_TA, S_TA, S_PRIMA, S_SHADEA, S_ENVA, S_1, S_0 };
static const u8 al_c[8] = { S_0, S_TA, S_TA, S_PRIMA, S_SHADEA, S_ENVA, S_0, S_0 };

/* What the current formula needs from OpenGL. Anything the texture isn't part
 * of is worked out per vertex here, so the unit only has three shapes to do:
 * replace, tex * colour, and a blend between two things by the texture. */
enum { M_VERTEX, M_MOD, M_LERP_T, M_LERP_C };
static int f_rgb, f_al;                  /* M_* */
static u8 ev_rgb[4], ev_al[4];           /* the a, b, c, d in use; S_* */
static u8 vp_rgb[4], vp_al[4];
static int use_tex, lerp_const_env;

static int is_tex(int s) { return s == S_T || s == S_TA; }

static void classify(int a, int b, int c, int d, int *mode, u8 *terms, u8 *expr) {
    terms[0] = a; terms[1] = b; terms[2] = c; terms[3] = d;
    if (!is_tex(a) && !is_tex(b) && !is_tex(c) && !is_tex(d)) {
        *mode = M_VERTEX;
        expr[0] = a; expr[1] = b; expr[2] = c; expr[3] = d;
        return;
    }
    if (d == S_0 && b == S_0 && is_tex(a) && !is_tex(c)) { *mode = M_MOD; expr[0] = c; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = a; return; }
    if (d == S_0 && b == S_0 && is_tex(c) && !is_tex(a)) { *mode = M_MOD; expr[0] = a; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = c; return; }
    if (d == b && is_tex(a) && is_tex(c)) { *mode = M_LERP_T; expr[0] = b; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = a; terms[2] = c; return; }
    if (d == b && is_tex(c)) { *mode = M_LERP_C; expr[0] = b; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = a; terms[2] = c; return; }
    if (is_tex(d) && a == b) { *mode = M_MOD; expr[0] = S_1; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = d; return; }
    if (is_tex(a) && b == S_0 && !is_tex(c) && !is_tex(d)) { *mode = M_MOD; expr[0] = c; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = a; f3d_stats.cc_approx++; return; }
    *mode = M_MOD; expr[0] = S_1; expr[1] = S_0; expr[2] = S_1; expr[3] = S_0; terms[0] = S_T;
    f3d_stats.cc_approx++;
    if (f3d_stats.cc_log < 32) {
        f3d_stats.cc_log++;
        fprintf(stderr, "f3d: combine %06X %08X (%d %d %d %d)\n", cc_hi & 0xFFFFFF, cc_lo, a, b, c, d);
    }
}

static void decode_combine(void) {
    const u32 h = cc_hi, l = cc_lo;
    /* in two cycles the second usually just passes the first on */
    int two = (oth_h >> 20 & 3) == 1;
    int a = h >> 20 & 15, c = h >> 15 & 31, b = l >> 28 & 15, d = l >> 15 & 7;
    int aa = h >> 12 & 7, ac = h >> 9 & 7, ab = l >> 12 & 7, ad = l >> 9 & 7;
    if (two) {
        const int a1 = h >> 5 & 15, c1 = h & 31, b1 = l >> 24 & 15, d1 = l >> 6 & 7;
        if (!(rgb_d[d1] == S_CMB && rgb_b[b1] == S_0 && rgb_c[c1] == S_0)) { a = a1; b = b1; c = c1; d = d1; }
        const int aa1 = l >> 3 & 7, ab1 = l >> 21 & 7, ac1 = l & 7, ad1 = l >> 18 & 7;
        if (!(al_abd[ad1] == S_CMB && al_abd[ab1] == S_0 && al_c[ac1] == S_0)) { aa = aa1; ab = ab1; ac = ac1; ad = ad1; }
    }
    classify(rgb_a[a], rgb_b[b], rgb_c[c], rgb_d[d], &f_rgb, ev_rgb, vp_rgb);
    classify(al_abd[aa], al_abd[ab], al_c[ac], al_abd[ad], &f_al, ev_al, vp_al);
    use_tex = f_rgb != M_VERTEX || f_al != M_VERTEX;
}

static inline int src_value(int s, const Vtx *o, int ch) {
    switch (s) {
    case S_0: return 0;
    case S_1: return 255;
    case S_SHADE: return ch == 0 ? o->r : ch == 1 ? o->g : ch == 2 ? o->b : (geom & 0x10000 ? 255 : o->a);
    case S_SHADEA: return geom & 0x10000 ? 255 : o->a;
    case S_PRIM: return c_prim[ch];
    case S_PRIMA: return c_prim[3];
    case S_ENV: return c_env[ch];
    case S_ENVA: return c_env[3];
    }
    return 0;
}

static inline u8 expr_value(const u8 *e, const Vtx *o, int ch) {
    const int a = src_value(e[0], o, ch), b = src_value(e[1], o, ch), c = src_value(e[2], o, ch), d = src_value(e[3], o, ch);
    const int r = (a - b) * c / 255 + d;
    return r < 0 ? 0 : r > 255 ? 255 : (u8)r;
}

/* ---------------------------------------------------------------- GL state */
static void set_combine(void) {
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    static const GLint rgb_mode[] = { GL_REPLACE, GL_MODULATE, GL_INTERPOLATE_EXT, GL_INTERPOLATE_EXT };
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, rgb_mode[f_rgb]);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_EXT, rgb_mode[f_al]);
    /* RGB */
    switch (f_rgb) {
    case M_VERTEX:
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PRIMARY_COLOR_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
        break;
    case M_MOD:
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, ev_rgb[0] == S_TA ? GL_SRC_ALPHA : GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
        break;
    case M_LERP_T:
    case M_LERP_C:
        if (f_rgb == M_LERP_T) {
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, ev_rgb[0] == S_TA ? GL_SRC_ALPHA : GL_SRC_COLOR);
        } else {
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_CONSTANT_EXT);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB_EXT, GL_SRC_COLOR);
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB_EXT, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB_EXT, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB_EXT, ev_rgb[2] == S_TA ? GL_SRC_ALPHA : GL_SRC_COLOR);
        break;
    }
    /* alpha */
    if (f_al == M_VERTEX) {
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_PRIMARY_COLOR_EXT);
    } else if (f_al == M_MOD) {
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_EXT, GL_PRIMARY_COLOR_EXT);
    } else {
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, f_al == M_LERP_T ? GL_TEXTURE : GL_CONSTANT_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_EXT, GL_PRIMARY_COLOR_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_ALPHA_EXT, GL_TEXTURE);
    }
    if (f_rgb == M_LERP_C || f_al == M_LERP_C) {
        const int s = ev_rgb[0], al = ev_al[0];
        GLfloat c[4];
        const u8 *rgb = s == S_PRIM ? c_prim : s == S_ENV ? c_env : c_env;
        c[0] = rgb[0] / 255.0f; c[1] = rgb[1] / 255.0f; c[2] = rgb[2] / 255.0f;
        c[3] = (al == S_PRIMA ? c_prim[3] : c_env[3]) / 255.0f;
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
    }
}

static void flush(void) {
    if (!b_n) return;
    glDrawArrays(GL_TRIANGLES, 0, b_n);
    f3d_stats.draws++;
    f3d_stats.tris += b_n / 3;
    b_n = 0;
}

static int fog_on, gl_fog = -1, gl_texon = -1, gl_blend = -1, gl_atest = -1, gl_ztest = -1, gl_zwrite = -1, gl_cull = -1, gl_decal = -1;
static float tw_cur = 32, th_cur = 32;
static int view_x, view_y, view_w = 640, view_h = 480;

static void fog_unit(int on) {
    if (on == gl_fog) return;
    gl_fog = on;
    glActiveTextureARB(GL_TEXTURE1_ARB);
    if (on) {
        GLfloat c[4] = { c_fog[0] / 255.0f, c_fog[1] / 255.0f, c_fog[2] / 255.0f, 1 };
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex_white);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_INTERPOLATE_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_CONSTANT_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PREVIOUS_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB_EXT, GL_PRIMARY_COLOR_EXT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB_EXT, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_EXT, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_EXT, GL_PREVIOUS_EXT);
    } else glDisable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0_ARB);
}

/* Everything between draws lands here, once, right before the next triangle. */
static void apply_state(int rect) {
    flush();
    dirty = 0;
    const int cycle = oth_h >> 20 & 3;
    decode_combine();
    if (rect < 0 && !tex_on) use_tex = 0; /* gSPTexture is about triangles only */
    if (cycle >= 2) {                     /* copy, fill: no combiner */
        use_tex = cycle == 2;
        f_rgb = f_al = cycle == 2 ? M_MOD : M_VERTEX;
        ev_rgb[0] = S_T; ev_al[0] = S_TA;
        vp_rgb[0] = vp_al[0] = S_1; vp_rgb[1] = vp_al[1] = S_0; vp_rgb[2] = vp_al[2] = S_1; vp_rgb[3] = vp_al[3] = S_0;
    }
    if (use_tex != gl_texon) {
        gl_texon = use_tex;
        if (use_tex) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    }
    if (use_tex) {
        tw_cur = bind_tile(&tiles[rect >= 0 ? rect : tex_tile], &th_cur);
        set_combine();
    }

    /* blender */
    const int m2a = cycle == 1 ? oth_l >> 20 & 3 : oth_l >> 22 & 3;
    const int blend = cycle < 2 && (oth_l & 0x4000) && m2a == 1;
    fog_on = cycle == 1 && (oth_l >> 30 & 3) == 3 && !(geom & 0x10000) == 0;
    if (blend != gl_blend) {
        gl_blend = blend;
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    }
    int at = 0;
    float ref = 0;
    if (cycle == 2) { at = (oth_l & 3) != 0; ref = 0; }   /* copy mode only asks for non-zero */
    else if ((oth_l & 3) == 1) { at = 1; ref = c_blend[3] ? (c_blend[3] - 1) / 255.0f : 0; }
    else if (oth_l & 0x1000) { at = 1; ref = 0.5f; }
    if (at != gl_atest) { gl_atest = at; if (at) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST); }
    if (at) glAlphaFunc(GL_GREATER, ref);

    const int zt = rect < 0 && cycle < 2 && (oth_l & 0x10), zw = rect < 0 && cycle < 2 && (oth_l & 0x20);
    if (zt != gl_ztest) { gl_ztest = zt; if (zt) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
    if (zw != gl_zwrite) { gl_zwrite = zw; glDepthMask(zw ? GL_TRUE : GL_FALSE); }
    const int decal = (oth_l >> 10 & 3) == 3;
    if (decal != gl_decal) {
        gl_decal = decal;
        if (decal) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1, -2); } else glDisable(GL_POLYGON_OFFSET_FILL);
    }
    int cull = rect >= -1 ? 0 : 0;
    if (rect < 0) cull = geom >> 12 & 3;
    if (cull != gl_cull) {
        gl_cull = cull;
        if (cull == 0) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(cull == 3 ? GL_FRONT_AND_BACK : cull == 2 ? GL_BACK : GL_FRONT); }
    }
    fog_unit(fog_on);
    glEnable(GL_SCISSOR_TEST);
    glScissor(view_x + sc_x0 * view_w / 320, view_y + (240 - sc_y1) * view_h / 240, (sc_x1 - sc_x0) * view_w / 320, (sc_y1 - sc_y0) * view_h / 240);
}

/* ---------------------------------------------------------------- triangles */
static inline void put(int i, const Vtx *o, const Vtx *c, const Tile *t) {
    float *p = b_pos[i];
    p[0] = o->x; p[1] = o->y; p[2] = o->z; p[3] = o->w;
    u8 *q = b_col[i];
    q[0] = expr_value(vp_rgb, c, 0); q[1] = expr_value(vp_rgb, c, 1); q[2] = expr_value(vp_rgb, c, 2);
    q[3] = fog_on ? o->a : expr_value(vp_al, c, 3);
    if (use_tex) {
        float s = (float)o->s * (float)tex_ss / 65536.0f, u = (float)o->t * (float)tex_st / 65536.0f;
        if (t->shifts) s = t->shifts <= 10 ? s / (float)(1 << t->shifts) : s * (float)(1 << (16 - t->shifts));
        if (t->shiftt) u = t->shiftt <= 10 ? u / (float)(1 << t->shiftt) : u * (float)(1 << (16 - t->shiftt));
        b_st[i][0] = (s / 32.0f - t->uls / 4.0f) / tw_cur;
        b_st[i][1] = (u / 32.0f - t->ult / 4.0f) / th_cur;
    }
}

static void tri(int a, int b, int c) {
    if (a >= 16 || b >= 16 || c >= 16) return;
    if (v[a].cc & v[b].cc & v[c].cc) return;
    if (dirty) apply_state(-1);
    if (b_n >= BATCH * 3) flush();
    const Tile *t = &tiles[tex_tile];
    const Vtx *flat = geom & 0x200 ? NULL : &v[a];
    put(b_n, &v[a], flat ? flat : &v[a], t);
    put(b_n + 1, &v[b], flat ? flat : &v[b], t);
    put(b_n + 2, &v[c], flat ? flat : &v[c], t);
    b_n += 3;
}

/* A screen rectangle in 10.2 pixels. */
static void rect(int tile, float x0, float y0, float x1, float y1, float s0, float t0, float ds, float dt, int flip) {
    dirty = 1;
    apply_state(tile);
    const float l = x0 / 160 - 1, r = x1 / 160 - 1, top = 1 - y0 / 120, bot = 1 - y1 / 120;
    const float s1 = s0 + (x1 - x0) * ds, t1 = t0 + (y1 - y0) * dt;
    Vtx q = { 0 };
    q.r = q.g = q.b = q.a = 255;
    const float sa = (s0 - 0) / tw_cur, sb = s1 / tw_cur, ta = t0 / th_cur, tb = t1 / th_cur;
    static const u8 ix[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) {
        const int c = ix[k], right = c == 1 || c == 2, down = c >= 2;
        float *p = b_pos[k];
        p[0] = right ? r : l; p[1] = down ? bot : top; p[2] = -1; p[3] = 1;
        b_st[k][0] = flip ? (down ? sb : sa) : (right ? sb : sa);
        b_st[k][1] = flip ? (right ? tb : ta) : (down ? tb : ta);
        u8 *o = b_col[k];
        o[0] = expr_value(vp_rgb, &q, 0); o[1] = expr_value(vp_rgb, &q, 1); o[2] = expr_value(vp_rgb, &q, 2); o[3] = expr_value(vp_al, &q, 3);
    }
    b_n = 6;
    flush();
    dirty = 1;
}

static void g_fillrect(u32 w0, u32 w1) {
    const int x0 = w1 >> 14 & 0x3FF, y0 = w1 >> 2 & 0x3FF, x1 = w0 >> 14 & 0x3FF, y1 = w0 >> 2 & 0x3FF;
    const int cycle = oth_h >> 20 & 3;
    if (cimg_addr == zimg_addr) {          /* the Z buffer is cleared by painting it */
        flush();
        glDepthMask(GL_TRUE); gl_zwrite = 1;
        glDisable(GL_SCISSOR_TEST);
        glClear(GL_DEPTH_BUFFER_BIT);
        dirty = 1;
        return;
    }
    if (cycle == 3) {
        const u32 c = c_fill >> 16;
        const u8 keep[4] = { c_prim[0], c_prim[1], c_prim[2], c_prim[3] };
        c_prim[0] = x5(c >> 11 & 31); c_prim[1] = x5(c >> 6 & 31); c_prim[2] = x5(c >> 1 & 31); c_prim[3] = 255;
        /* fill mode draws the prim-coloured path below */
        const u32 kh = cc_hi, kl = cc_lo;
        cc_hi = (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) ; cc_lo = (15u << 28) | (3u << 15) | (7u << 12) | (3u << 9);
        const u32 ko = oth_h;
        oth_h &= ~(3u << 20);
        rect(0, (float)x0, (float)y0, (float)x1 + 1, (float)y1 + 1, 0, 0, 0, 0, 0);
        oth_h = ko; cc_hi = kh; cc_lo = kl;
        memcpy(c_prim, keep, 4);
    } else {
        rect(0, (float)x0, (float)y0, (float)x1, (float)y1, 0, 0, 0, 0, 0);
    }
}

/* ---------------------------------------------------------------- loads */
static void load_block(u32 w0, u32 w1) {
    Tile *t = &tiles[w1 >> 24 & 7];
    const int uls = w0 >> 12 & 0xFFF, ult = w0 & 0xFFF, lrs = w1 >> 12 & 0xFFF;
    const int texels = lrs - uls + 1;
    const u8 *src = &n64_rdram[(timg_addr + ((u32)ult * timg_w + uls) * (1 << timg_siz) / 2) & 0x7FFFFF];
    u8 *dst = &tmem[(t->tmem * 8) & 0xFFF];
    tmem_gen++;
    if (t->siz == 3) {
        const int base = (t->tmem * 8) & 0x7FF;
        for (int i = 0; i < texels; i++) {
            const int a = (base + i * 2) & 0x7FF;
            tmem[a] = src[i * 4]; tmem[a + 1] = src[i * 4 + 1];
            tmem[a + 0x800] = src[i * 4 + 2]; tmem[a + 0x801] = src[i * 4 + 3];
        }
        return;
    }
    int bytes = (texels << t->siz) >> 1;
    if (bytes > 4096 - (t->tmem * 8)) bytes = 4096 - t->tmem * 8;
    if (bytes > 0) memcpy(dst, src, (size_t)bytes);
}

static void load_tile(u32 w0, u32 w1) {
    Tile *t = &tiles[w1 >> 24 & 7];
    const int uls = (w0 >> 12 & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2, lrs = (w1 >> 12 & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
    const int stride = (timg_w << timg_siz) >> 1, line = t->line * 8, bpp = 1 << timg_siz;
    tmem_gen++;
    for (int y = ult; y <= lrt; y++) {
        const u8 *src = &n64_rdram[(timg_addr + y * stride + uls * bpp / 2) & 0x7FFFFF];
        const int off = t->tmem * 8 + (y - ult) * line;
        int n = (lrs - uls + 1) * bpp / 2;
        if (off >= 4096) break;
        if (off + n > 4096) n = 4096 - off;
        memcpy(&tmem[off], src, (size_t)n);
    }
}

static void load_tlut(u32 w1) {
    const Tile *t = &tiles[w1 >> 24 & 7];
    const int n = ((w1 >> 14 & 0x3FF) - 0) + 1;
    const u8 *src = &n64_rdram[timg_addr & 0x7FFFFF];
    int i = (t->tmem - 256) & 255;
    tmem_gen++;
    for (int k = 0; k < n && i < 256; k++, i++) tlut[i] = R16(src + k * 2);
}

/* ---------------------------------------------------------------- the list */
static void g_movemem(u32 w0, u32 w1) {
    const int i = w0 >> 16 & 0xFF;
    const u8 *p = &n64_rdram[addr(w1)];
    if (i == 0x80) {
        const float sx = (s16)R16(p) / 4.0f, sy = (s16)R16(p + 2) / 4.0f, tx = (s16)R16(p + 8) / 4.0f, ty = (s16)R16(p + 10) / 4.0f;
        vp_sx = sx / 160; vp_sy = sy / 120; vp_tx = (tx - 160) / 160; vp_ty = (120 - ty) / 120;
    } else if (i == 0x82 || i == 0x84) {
        s8 *o = look_raw[i == 0x84 ? 0 : 1];
        o[0] = (s8)p[8]; o[1] = (s8)p[9]; o[2] = (s8)p[10];
        lights_ok = 0;
    } else if (i >= 0x86 && i <= 0x94) {
        const int l = (i - 0x86) / 2;
        l_col[l][0] = p[0]; l_col[l][1] = p[1]; l_col[l][2] = p[2];
        if (l < 8) { l_raw[l][0] = (s8)p[8]; l_raw[l][1] = (s8)p[9]; l_raw[l][2] = (s8)p[10]; }
        lights_ok = 0;
    }
}

static void g_moveword(u32 w0, u32 w1) {
    const int i = w0 & 0xFF, off = w0 >> 8 & 0xFFFF;
    if (i == 6) seg[off >> 2 & 15] = w1 & 0xFFFFFF;
    else if (i == 2) { n_lights = (int)((w1 & 0xFFFF) / 32) - 1; if (n_lights < 0) n_lights = 0; if (n_lights > 7) n_lights = 7; lights_ok = 0; }
    else if (i == 8) { fog_mul = (float)(s16)(w1 >> 16); fog_off = (float)(s16)w1; }
}

static void set_tile(u32 w0, u32 w1) {
    Tile *t = &tiles[w1 >> 24 & 7];
    t->fmt = w0 >> 21 & 7; t->siz = w0 >> 19 & 3; t->line = w0 >> 9 & 0x1FF; t->tmem = w0 & 0x1FF;
    t->pal = w1 >> 20 & 15; t->cmt = w1 >> 18 & 3; t->maskt = w1 >> 14 & 15; t->shiftt = w1 >> 10 & 15;
    t->cms = w1 >> 8 & 3; t->masks = w1 >> 4 & 15; t->shifts = w1 & 15;
    dirty = 1;
}

static void walk(u32 a, int depth) {
    for (int guard = 0; guard < 1000000; guard++) {
        const u8 *p = &n64_rdram[addr(a)];
        const u32 w0 = R32(p), w1 = R32(p + 4);
        a += 8;
        switch (w0 >> 24) {
        case 0x01: g_mtx(w0, w1); break;
        case 0x03: g_movemem(w0, w1); break;
        case 0x04: g_vtx(w0, w1); break;
        case 0x06:
            if (depth < 16) walk(w1, depth + 1);
            if (w0 >> 16 & 1) return;
            break;
        case 0xB6: geom &= ~w1; dirty = 1; break;
        case 0xB7: geom |= w1; dirty = 1; break;
        case 0xB8: return;
        case 0xB9: case 0xBA: {
            const u32 sh = w0 >> 8 & 0xFF, len = w0 & 0xFF, m = (len >= 32 ? ~0u : (1u << len) - 1) << sh;
            u32 *o = (w0 >> 24) == 0xB9 ? &oth_l : &oth_h;
            *o = (*o & ~m) | (w1 & m);
            dirty = 1;
            break;
        }
        case 0xBB:
            tex_on = w0 & 1; tex_tile = w0 >> 8 & 7;
            tex_ss = w1 >> 16; tex_st = w1 & 0xFFFF;
            dirty = 1;
            break;
        case 0xBC: g_moveword(w0, w1); break;
        case 0xBD: if (mv_top > 0) { mv_top--; mvp_ok = 0; lights_ok = 0; } break;
        case 0xBE: {
            int all = 0x3F;
            const int first = (w0 & 0xFFF) / 40, last = (w1 & 0xFFF) / 40;
            for (int i = first; i <= last && i < 16; i++) all &= v[i].cc;
            if (all) return;
            break;
        }
        case 0xBF: tri((w1 >> 16 & 0xFF) / 10, (w1 >> 8 & 0xFF) / 10, (w1 & 0xFF) / 10); break;
        case 0xE4: case 0xE5: {
            const u8 *q = &n64_rdram[addr(a)];
            const u32 s_t = R32(q + 4), ds_dt = R32(q + 12);
            a += 16;
            const float x1 = (w0 >> 12 & 0xFFF) / 4.0f, y1 = (w0 & 0xFFF) / 4.0f;
            const float x0 = (w1 >> 12 & 0xFFF) / 4.0f, y0 = (w1 & 0xFFF) / 4.0f;
            const int tile = w1 >> 24 & 7, copy = (oth_h >> 20 & 3) == 2;
            const Tile *t = &tiles[tile];
            float s = (s16)(s_t >> 16) / 32.0f - t->uls / 4.0f, u = (s16)s_t / 32.0f - t->ult / 4.0f;
            float ds = (s16)(ds_dt >> 16) / 1024.0f, dt = (s16)ds_dt / 1024.0f;
            if (copy) ds /= 4;
            rect(tile, x0, y0, x1 + (copy ? 1 : 0), y1 + (copy ? 1 : 0), s, u, ds, dt, (w0 >> 24) == 0xE5);
            break;
        }
        case 0xE7: case 0xE8: case 0xE9: case 0xE6: break;
        case 0xED: flush(); dirty = 1;
            sc_x0 = (w0 >> 12 & 0xFFF) / 4; sc_y0 = (w0 & 0xFFF) / 4;
            sc_x1 = (w1 >> 12 & 0xFFF) / 4; sc_y1 = (w1 & 0xFFF) / 4;
            break;
        case 0xEF: oth_h = (oth_h & 0xFF000000u) | (w0 & 0xFFFFFF); oth_l = w1; dirty = 1; break;
        case 0xF0: load_tlut(w1); dirty = 1; break;
        case 0xF2: {
            Tile *t = &tiles[w1 >> 24 & 7];
            t->uls = w0 >> 12 & 0xFFF; t->ult = w0 & 0xFFF; t->lrs = w1 >> 12 & 0xFFF; t->lrt = w1 & 0xFFF;
            dirty = 1;
            break;
        }
        case 0xF3: load_block(w0, w1); dirty = 1; break;
        case 0xF4: load_tile(w0, w1); dirty = 1; break;
        case 0xF5: set_tile(w0, w1); break;
        case 0xF6: g_fillrect(w0, w1); break;
        case 0xF7: c_fill = w1; break;
        case 0xF8: c_fog[0] = w1 >> 24; c_fog[1] = w1 >> 16; c_fog[2] = w1 >> 8; c_fog[3] = w1; dirty = 1; gl_fog = -1; break;
        case 0xF9: c_blend[0] = w1 >> 24; c_blend[1] = w1 >> 16; c_blend[2] = w1 >> 8; c_blend[3] = w1; dirty = 1; break;
        case 0xFA: c_prim[0] = w1 >> 24; c_prim[1] = w1 >> 16; c_prim[2] = w1 >> 8; c_prim[3] = w1; dirty = 1; break;
        case 0xFB: c_env[0] = w1 >> 24; c_env[1] = w1 >> 16; c_env[2] = w1 >> 8; c_env[3] = w1; dirty = 1; break;
        case 0xFC: cc_hi = w0; cc_lo = w1; dirty = 1; break;
        case 0xFD: timg_addr = addr(w1); timg_siz = w0 >> 19 & 3; timg_w = (w0 & 0xFFF) + 1; break;
        case 0xFE: zimg_addr = addr(w1); break;
        case 0xFF: flush(); cimg_addr = addr(w1); dirty = 1; break;
        }
    }
}

void f3d_init(void) {
    static const u8 white[4] = { 255, 255, 255, 255 };
    gl_fog = gl_texon = gl_blend = gl_atest = gl_ztest = gl_zwrite = gl_cull = gl_decal = -1;   /* a new context knows nothing */
    glGenTextures(1, (GLuint *)&tex_white);
    glBindTexture(GL_TEXTURE_2D, tex_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_LEQUAL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, b_pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, b_col);
    glTexCoordPointer(2, GL_FLOAT, 0, b_st);
    f3d_view(0, 0, 640, 480);
    sc_x1 = 320; sc_y1 = 240;
    vp_sx = vp_sy = 1;
}

void f3d_view(int x, int y, int w, int h) {
    view_x = x; view_y = y; view_w = w; view_h = h;
    glViewport(x, y, w, h);
}

void f3d_overlay(const u32 *px, int w, int h, int stride, int fresh) {
    static u8 t[32][256][4];
    static GLuint id;
    flush();
    if (!id) {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        fresh = 1;
    } else glBindTexture(GL_TEXTURE_2D, id);
    if (fresh) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const u32 c = px[y * stride + x];
                t[y][x][0] = c >> 16; t[y][x][1] = c >> 8; t[y][x][2] = c; t[y][x][3] = 255;
            }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
    }
    /* the game's next draw sets all of this again */
    fog_unit(0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_TEXTURE_2D);
    gl_texon = 1; gl_blend = gl_atest = gl_ztest = gl_cull = gl_decal = 0;
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    const float x0 = -1 + 8.0f / 640, x1 = x0 + w * 4.0f / 640, y0 = 1 - 8.0f / 480, y1 = y0 - h * 4.0f / 480;
    const float s = w / 256.0f, u = h / 32.0f;
    const float v[6][4] = { { x0, y0, 0, 0 }, { x0, y1, 0, u }, { x1, y0, s, 0 }, { x1, y0, s, 0 }, { x0, y1, 0, u }, { x1, y1, s, u } };
    for (int i = 0; i < 6; i++) {
        b_pos[i][0] = v[i][0]; b_pos[i][1] = v[i][1]; b_pos[i][2] = 0; b_pos[i][3] = 1;
        b_st[i][0] = v[i][2]; b_st[i][1] = v[i][3];
        memset(b_col[i], 255, 4);
    }
    b_n = 6;
    flush();
    dirty = 1;
}

void f3d_run(u32 dl) {
    mv_top = 0;
    mvp_ok = lights_ok = 0;
    memset(seg, 0, sizeof seg);
    dirty = 1;
    f3d_stats.frames++;
    walk(dl, 0);
    flush();
}

/*
 * ps1_gpu.c - the GPU: its command FIFO, VRAM transfers, and a rasterizer for
 * polygons, lines and rectangles with textures, CLUTs, semi-transparency,
 * dithering and the mask bit, at the console's own resolution.
 */
#include <stdlib.h>
#include "ps1.h"

u16 vram[1024 * 512];
int gpu_dot_div = 10;
int gpu_trace;

static u32 cmd[16];
static int ncmd, need;
static int polyline;                    /* inside a line strip, waiting for its end marker */

static u32 tpage;                       /* GP0(E1) */
static int twin_mx, twin_my, twin_ox, twin_oy;
static int dx1, dy1, dx2, dy2, ox, oy;
static int mask_set, mask_check;
static int dither, draw_display, tex_disable, rect_flip_x, rect_flip_y;

static int disp_x, disp_y, h1 = 0x200, h2 = 0xc00, v1 = 0x10, v2 = 0x100;
static u32 dmode;
static int disp_off = 1, dma_dir, gpu_irq;
static int line_now, field;
static u32 info;                        /* GP1(10) answer */

static struct { int x, y, w, h, i, n; } up, down;   /* CPU to VRAM, VRAM to CPU */

static const s8 dither_table[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

void gpu_reset(void) {
    tpage = 0; twin_mx = twin_my = twin_ox = twin_oy = 0;
    dx1 = dy1 = 0; dx2 = dy2 = 0; ox = oy = 0;
    mask_set = mask_check = 0;
    dmode = 0; disp_off = 1; dma_dir = 0; gpu_irq = 0;
    ncmd = need = 0;
    gpu_dot_div = 10;
}

/* ---- pixels ---- */

static inline void plot(int x, int y, u16 c, int semi, int mode) {
    u16 *p = &vram[y * 1024 + x];
    if (mask_check && (*p & 0x8000)) return;
    if (semi) {
        const int br = *p & 31, bg = (*p >> 5) & 31, bb = (*p >> 10) & 31;
        int fr = c & 31, fg = (c >> 5) & 31, fb = (c >> 10) & 31;
        switch (mode) {
        case 0: fr = (br + fr) >> 1; fg = (bg + fg) >> 1; fb = (bb + fb) >> 1; break;
        case 1: fr += br; fg += bg; fb += bb; break;
        case 2: fr = br - fr; fg = bg - fg; fb = bb - fb; break;
        case 3: fr = br + (fr >> 2); fg = bg + (fg >> 2); fb = bb + (fb >> 2); break;
        }
        fr = fr < 0 ? 0 : fr > 31 ? 31 : fr;
        fg = fg < 0 ? 0 : fg > 31 ? 31 : fg;
        fb = fb < 0 ? 0 : fb > 31 ? 31 : fb;
        c = (c & 0x8000) | fr | fg << 5 | fb << 10;
    }
    *p = c | (mask_set << 15);
}

static inline u16 texel(int u, int v, u32 page, u16 clut) {
    u = ((u & ~(twin_mx * 8)) | ((twin_ox & twin_mx) * 8)) & 0xff;
    v = ((v & ~(twin_my * 8)) | ((twin_oy & twin_my) * 8)) & 0xff;
    const int tx = (page & 15) * 64, ty = ((page >> 4) & 1) * 256, depth = (page >> 7) & 3;
    const int cx = (clut & 0x3f) * 16, cy = (clut >> 6) & 0x1ff;
    const int row = ((ty + v) & 511) * 1024;
    switch (depth) {
    case 0: { const u16 w = vram[row + ((tx + (u >> 2)) & 1023)]; return vram[cy * 1024 + ((cx + ((w >> ((u & 3) * 4)) & 15)) & 1023)]; }
    case 1: { const u16 w = vram[row + ((tx + (u >> 1)) & 1023)]; return vram[cy * 1024 + ((cx + ((w >> ((u & 1) * 8)) & 0xff)) & 1023)]; }
    default: return vram[row + ((tx + u) & 1023)];
    }
}

/* a color from 8-bit channels, dithered or not, with the texel's own bit 15 */
static inline u16 shade(int r, int g, int b, int x, int y, int dith) {
    if (dith) { const int d = dither_table[y & 3][x & 3]; r += d; g += d; b += d; }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (r >> 3) | (g >> 3) << 5 | (b >> 3) << 10;
}

typedef struct { s32 x, y; s32 r, g, b, u, v; } Vtx;

enum { F_GOURAUD = 1, F_TEX = 2, F_SEMI = 4, F_RAW = 8 };

static inline s64 edge(const Vtx *a, const Vtx *b, s32 x, s32 y) {
    return (s64)(b->x - a->x) * (y - a->y) - (s64)(b->y - a->y) * (x - a->x);
}

static void triangle(Vtx p0, Vtx p1, Vtx p2, int flags, u16 clut, u32 page) {
    Vtx *v0 = &p0, *v1 = &p1, *v2 = &p2;
    s64 area = edge(v0, v1, v2->x, v2->y);
    if (area == 0) return;
    if (area < 0) { Vtx *t = v1; v1 = v2; v2 = t; area = -area; }
    int minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    const Vtx *vs[3] = { v0, v1, v2 };
    for (int i = 1; i < 3; i++) {
        if (vs[i]->x < minx) minx = vs[i]->x;
        if (vs[i]->x > maxx) maxx = vs[i]->x;
        if (vs[i]->y < miny) miny = vs[i]->y;
        if (vs[i]->y > maxy) maxy = vs[i]->y;
    }
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    if (minx < dx1) minx = dx1;
    if (miny < dy1) miny = dy1;
    if (maxx > dx2) maxx = dx2;
    if (maxy > dy2) maxy = dy2;
    if (minx > maxx || miny > maxy) return;

    /* attribute gradients, 16.16 */
    const s32 ex1 = v1->x - v0->x, ey1 = v1->y - v0->y, ex2 = v2->x - v0->x, ey2 = v2->y - v0->y;
    #define GRAD(f, ddx, ddy) const s64 ddx = (((s64)(v1->f - v0->f) * ey2 - (s64)(v2->f - v0->f) * ey1) << 16) / area, \
                                    ddy = (((s64)(v2->f - v0->f) * ex1 - (s64)(v1->f - v0->f) * ex2) << 16) / area
    GRAD(r, rdx, rdy); GRAD(g, gdx, gdy); GRAD(b, bdx, bdy); GRAD(u, udx, udy); GRAD(v, vdx, vdy);
    #undef GRAD

    /* the top-left rule: right and bottom edges are left out */
    #define BIAS(a, b) (((b)->y < (a)->y) || ((b)->y == (a)->y && (b)->x > (a)->x) ? 0 : -1)
    const s64 b0 = BIAS(v1, v2), b1 = BIAS(v2, v0), b2 = BIAS(v0, v1);
    #undef BIAS
    const int semi_mode = (page >> 5) & 3;
    const int dith = dither && (flags & F_GOURAUD || (flags & (F_TEX | F_RAW)) == F_TEX);
    for (int y = miny; y <= maxy; y++) {
        s64 w0 = edge(v1, v2, minx, y), w1 = edge(v2, v0, minx, y), w2 = edge(v0, v1, minx, y);
        const s64 s0 = -(s64)(v2->y - v1->y), s1 = -(s64)(v0->y - v2->y), s2 = -(s64)(v1->y - v0->y);
        const s64 fx = minx - v0->x, fy = y - v0->y;
        s64 r = ((s64)v0->r << 16) + rdx * fx + rdy * fy + 0x8000, g = ((s64)v0->g << 16) + gdx * fx + gdy * fy + 0x8000;
        s64 b = ((s64)v0->b << 16) + bdx * fx + bdy * fy + 0x8000;
        s64 u = ((s64)v0->u << 16) + udx * fx + udy * fy + 0x8000, v = ((s64)v0->v << 16) + vdx * fx + vdy * fy + 0x8000;
        for (int x = minx; x <= maxx; x++, w0 += s0, w1 += s1, w2 += s2, r += rdx, g += gdx, b += bdx, u += udx, v += vdx) {
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            const int cr = flags & F_GOURAUD ? (int)(r >> 16) : v0->r, cg = flags & F_GOURAUD ? (int)(g >> 16) : v0->g;
            const int cb = flags & F_GOURAUD ? (int)(b >> 16) : v0->b;
            if (flags & F_TEX) {
                const u16 t = texel((int)(u >> 16), (int)(v >> 16), page, clut);
                if (!t) continue;
                u16 c;
                if (flags & F_RAW) c = t & 0x7fff;
                else c = shade(((t & 31) << 3) * cr >> 7, (((t >> 5) & 31) << 3) * cg >> 7, (((t >> 10) & 31) << 3) * cb >> 7, x, y, dith);
                plot(x, y, c | (t & 0x8000), (flags & F_SEMI) && (t & 0x8000), semi_mode);
            } else plot(x, y, shade(cr, cg, cb, x, y, dith), flags & F_SEMI, (tpage >> 5) & 3);
        }
    }
}

static void line(Vtx a, Vtx b, int gouraud, int semi) {
    const int ddx = b.x - a.x, ddy = b.y - a.y;
    if (abs(ddx) >= 1024 || abs(ddy) >= 512) return;
    const int n = abs(ddx) > abs(ddy) ? abs(ddx) : abs(ddy);
    for (int i = 0; i <= n; i++) {
        const int x = a.x + (n ? (ddx * i * 2 + (ddx >= 0 ? n : -n)) / (2 * n) : 0);
        const int y = a.y + (n ? (ddy * i * 2 + (ddy >= 0 ? n : -n)) / (2 * n) : 0);
        if (x < dx1 || x > dx2 || y < dy1 || y > dy2) continue;
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud && n) { r += (b.r - a.r) * i / n; g += (b.g - a.g) * i / n; bl += (b.b - a.b) * i / n; }
        plot(x, y, shade(r, g, bl, x, y, dither && gouraud), semi, (tpage >> 5) & 3);
    }
}

static void rect(int x, int y, int w, int h, int r, int g, int b, int flags, int u0, int v0, u16 clut) {
    for (int j = 0; j < h; j++) {
        const int py = y + j;
        if (py < dy1 || py > dy2) continue;
        for (int i = 0; i < w; i++) {
            const int px = x + i;
            if (px < dx1 || px > dx2) continue;
            if (flags & F_TEX) {
                const int u = rect_flip_x ? u0 - i : u0 + i, v = rect_flip_y ? v0 - j : v0 + j;
                const u16 t = texel(u, v, tpage, clut);
                if (!t) continue;
                const u16 c = flags & F_RAW ? t & 0x7fff
                    : shade(((t & 31) << 3) * r >> 7, (((t >> 5) & 31) << 3) * g >> 7, (((t >> 10) & 31) << 3) * b >> 7, px, py, 0);
                plot(px, py, c | (t & 0x8000), (flags & F_SEMI) && (t & 0x8000), (tpage >> 5) & 3);
            } else plot(px, py, shade(r, g, b, px, py, 0), flags & F_SEMI, (tpage >> 5) & 3);
        }
    }
}

/* ---- commands ---- */

static inline s32 sx11(u32 v) { return (s32)(v << 21) >> 21; }

static void vertex(Vtx *v, u32 xy) { v->x = sx11(xy) + ox; v->y = sx11(xy >> 16) + oy; }
static void color(Vtx *v, u32 c) { v->r = c & 0xff; v->g = (c >> 8) & 0xff; v->b = (c >> 16) & 0xff; }

static void set_tpage(u32 v) {
    tpage = v & 0x1ff;
    tex_disable = (v >> 11) & 1;
    dither = (v >> 9) & 1;
    draw_display = (v >> 10) & 1;
    rect_flip_x = (v >> 12) & 1;
    rect_flip_y = (v >> 13) & 1;
}

static void polygon(void) {
    const u32 c0 = cmd[0];
    const int quad = (c0 >> 27) & 1, tex = (c0 >> 26) & 1, gouraud = (c0 >> 28) & 1;
    int flags = (gouraud ? F_GOURAUD : 0) | (tex ? F_TEX : 0) | (c0 & (1 << 25) ? F_SEMI : 0) | (tex && (c0 & (1 << 24)) ? F_RAW : 0);
    Vtx v[4];
    int k = 0;
    u16 clut = 0;
    u32 page = tpage;
    for (int i = 0; i < 3 + quad; i++) {
        color(&v[i], i == 0 || !gouraud ? c0 : cmd[k]);
        if (i == 0) k = 1; else if (gouraud) k++;
        vertex(&v[i], cmd[k++]);
        if (tex) {
            const u32 t = cmd[k++];
            v[i].u = t & 0xff; v[i].v = (t >> 8) & 0xff;
            if (i == 0) clut = t >> 16;
            if (i == 1) page = (t >> 16) & 0x9ff;
        } else v[i].u = v[i].v = 0;
    }
    if (tex) { tpage = (tpage & ~0x1ffu) | (page & 0x1ff); tex_disable = (page >> 11) & 1; }
    if (tex && !gouraud && (flags & F_RAW) == 0) { /* flat shading modulates by the one color */ }
    if (!gouraud) flags &= ~F_GOURAUD;
    triangle(v[0], v[1], v[2], flags, clut, page);
    if (quad) triangle(v[1], v[2], v[3], flags, clut, page);
}

static int poly_words(u32 c) {
    const int quad = (c >> 27) & 1, tex = (c >> 26) & 1, gouraud = (c >> 28) & 1, n = 3 + quad;
    return 1 + n * (1 + tex) + (gouraud ? n - 1 : 0);
}

static void fill(void) {
    const u32 c = cmd[0];
    const u16 col = ((c >> 3) & 31) | ((c >> 11) & 31) << 5 | ((c >> 19) & 31) << 10;
    const int x = cmd[1] & 0x3f0, y = (cmd[1] >> 16) & 0x1ff;
    const int w = ((cmd[2] & 0x3ff) + 15) & ~15, h = (cmd[2] >> 16) & 0x1ff;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = col;
}

static void copy(void) {
    const int sx = cmd[1] & 0x3ff, sy = (cmd[1] >> 16) & 0x1ff, tx = cmd[2] & 0x3ff, ty = (cmd[2] >> 16) & 0x1ff;
    const int w = ((cmd[3] - 1) & 0x3ff) + 1, h = (((cmd[3] >> 16) - 1) & 0x1ff) + 1;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const u16 s = vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
            u16 *d = &vram[((ty + j) & 511) * 1024 + ((tx + i) & 1023)];
            if (mask_check && (*d & 0x8000)) continue;
            *d = s | (mask_set << 15);
        }
}

static void rectangle(void) {
    const u32 c0 = cmd[0];
    const int tex = (c0 >> 26) & 1, size = (c0 >> 27) & 3;
    int k = 1;
    const int x = sx11(cmd[k]) + ox, y = sx11(cmd[k] >> 16) + oy;
    k++;
    int u = 0, v = 0;
    u16 clut = 0;
    if (tex) { u = cmd[k] & 0xff; v = (cmd[k] >> 8) & 0xff; clut = cmd[k] >> 16; k++; }
    int w, h;
    if (size == 0) { w = cmd[k] & 0x3ff; h = (cmd[k] >> 16) & 0x1ff; }
    else w = h = size == 1 ? 1 : size == 2 ? 8 : 16;
    const int flags = (tex ? F_TEX : 0) | (c0 & (1 << 25) ? F_SEMI : 0) | (tex && (c0 & (1 << 24)) ? F_RAW : 0);
    rect(x, y, w, h, c0 & 0xff, (c0 >> 8) & 0xff, (c0 >> 16) & 0xff, flags, u, v, clut);
}

static void render(void) {
    const u32 op = cmd[0] >> 24;
    if (op >= 0x20 && op < 0x40) polygon();
    else if (op >= 0x60 && op < 0x80) rectangle();
    else if (op == 0x02) fill();
    else if (op >= 0x80 && op < 0xa0) copy();
    else if (op >= 0xa0 && op < 0xc0) {
        up.x = cmd[1] & 0x3ff; up.y = (cmd[1] >> 16) & 0x1ff;
        up.w = ((cmd[2] - 1) & 0x3ff) + 1; up.h = (((cmd[2] >> 16) - 1) & 0x1ff) + 1;
        up.i = 0; up.n = up.w * up.h;
        if (getenv("PS1_TRACE_UP") && up.y + up.h > 500) fprintf(stderr, "upload %d,%d %dx%d at %llu\n", up.x, up.y, up.w, up.h, (unsigned long long)ps1_now);
    } else if (op >= 0xc0 && op < 0xe0) {
        down.x = cmd[1] & 0x3ff; down.y = (cmd[1] >> 16) & 0x1ff;
        down.w = ((cmd[2] - 1) & 0x3ff) + 1; down.h = (((cmd[2] >> 16) - 1) & 0x1ff) + 1;
        down.i = 0; down.n = down.w * down.h;
    } else switch (op) {
    case 0xe1: set_tpage(cmd[0]); break;
    case 0xe2: twin_mx = cmd[0] & 31; twin_my = (cmd[0] >> 5) & 31; twin_ox = (cmd[0] >> 10) & 31; twin_oy = (cmd[0] >> 15) & 31; break;
    case 0xe3: dx1 = cmd[0] & 0x3ff; dy1 = (cmd[0] >> 10) & 0x1ff; break;
    case 0xe4: dx2 = cmd[0] & 0x3ff; dy2 = (cmd[0] >> 10) & 0x1ff; break;
    case 0xe5: ox = sx11(cmd[0]); oy = sx11(cmd[0] >> 11); break;
    case 0xe6: mask_set = cmd[0] & 1; mask_check = (cmd[0] >> 1) & 1; break;
    case 0x1f: gpu_irq = 1; irq_raise(1); break;
    }
}

static int words_for(u32 c) {
    const u32 op = c >> 24;
    if (op >= 0x20 && op < 0x40) return poly_words(c);
    if (op >= 0x40 && op < 0x60) return (op & 0x08) ? -1 : (op & 0x10) ? 4 : 3;
    if (op >= 0x60 && op < 0x80) return 2 + ((op >> 2) & 1) + (((op >> 3) & 3) == 0);
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xa0) return 4;
    if (op >= 0xa0 && op < 0xe0) return 3;
    return 1;
}

static Vtx pl_last;
static int pl_gouraud, pl_semi, pl_have_color;
static u32 pl_color;

void gpu_gp0(u32 v) {
    if (up.n) {         /* pixels for VRAM, two to a word */
        for (int k = 0; k < 2 && up.n; k++, up.n--, up.i++) {
            const u16 px = v >> (16 * k);
            const int x = (up.x + up.i % up.w) & 1023, y = (up.y + up.i / up.w) & 511;
            u16 *d = &vram[y * 1024 + x];
            if (mask_check && (*d & 0x8000)) continue;
            *d = px | (mask_set << 15);
        }
        return;
    }
    if (polyline) {
        if ((v & 0xf000f000) == 0x50005000 && !(pl_gouraud && pl_have_color)) { polyline = 0; return; }
        if (pl_gouraud && !pl_have_color) { pl_color = v; pl_have_color = 1; return; }
        Vtx n;
        color(&n, pl_color);
        vertex(&n, v);
        line(pl_last, n, pl_gouraud, pl_semi);
        pl_last = n;
        pl_have_color = 0;
        return;
    }
    if (ncmd == 0) {
        need = words_for(v);
        if (need < 0) {     /* a line strip: draw as the points come */
            const int g = (v >> 28) & 1;
            cmd[ncmd++] = v;        /* kept twice, so the points follow at the same places */
            need = g ? 5 : 4;
        }
    }
    cmd[ncmd++] = v;
    if (ncmd > 15) ncmd = 15;
    if (ncmd < need) return;
    const u32 op = cmd[0] >> 24;
    if (op >= 0x40 && op < 0x60) {
        const int strip = (op & 0x08) != 0, g = (op & 0x10) != 0;
        const u32 *c = strip ? cmd + 1 : cmd;
        Vtx a, b;
        color(&a, c[0]); vertex(&a, c[1]);
        color(&b, g ? c[2] : c[0]); vertex(&b, c[g ? 3 : 2]);
        line(a, b, g, (op & 0x02) != 0);
        if (strip) { polyline = 1; pl_last = b; pl_gouraud = g; pl_semi = (op & 0x02) != 0; pl_color = c[0]; pl_have_color = 0; }
        ncmd = 0;
        return;
    }
    if (gpu_trace) { fprintf(stderr, "gp0"); for (int i = 0; i < ncmd; i++) fprintf(stderr, " %08x", cmd[i]); fprintf(stderr, "\n"); }
    render();
    ncmd = 0;
}

void gpu_gp1(u32 v) {
    switch (v >> 24) {
    case 0x00:
        gpu_reset();
        disp_x = disp_y = 0; h1 = 0x200; h2 = 0xc00; v1 = 0x10; v2 = 0x100;
        up.n = down.n = 0; polyline = 0;
        break;
    case 0x01: ncmd = 0; up.n = 0; polyline = 0; break;
    case 0x02: gpu_irq = 0; break;
    case 0x03: disp_off = v & 1; break;
    case 0x04: dma_dir = v & 3; break;
    case 0x05: disp_x = v & 0x3fe; disp_y = (v >> 10) & 0x1ff; break;
    case 0x06: h1 = v & 0xfff; h2 = (v >> 12) & 0xfff; break;
    case 0x07: v1 = v & 0x3ff; v2 = (v >> 10) & 0x3ff; break;
    case 0x08: {
        dmode = v & 0xff;
        static const int divs[4] = { 10, 8, 5, 4 };
        gpu_dot_div = dmode & 0x40 ? 7 : divs[dmode & 3];
        break;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        switch (v & 7) {
        case 2: info = twin_mx | twin_my << 5 | twin_ox << 10 | (u32)twin_oy << 15; break;
        case 3: info = dx1 | dy1 << 10; break;
        case 4: info = dx2 | dy2 << 10; break;
        case 5: info = (ox & 0x7ff) | (u32)(oy & 0x7ff) << 11; break;
        case 7: info = 2; break;
        }
        break;
    }
}

u32 gpu_read(void) {
    if (!down.n) return info;
    u32 v = 0;
    for (int k = 0; k < 2 && down.n; k++, down.n--, down.i++) {
        const int x = (down.x + down.i % down.w) & 1023, y = (down.y + down.i / down.w) & 511;
        v |= (u32)vram[y * 1024 + x] << (16 * k);
    }
    return v;
}

u32 gpu_stat(void) {
    const int interlace = (dmode >> 5) & 1;
    u32 s = tpage & 0x1ff;
    s |= (u32)dither << 9 | (u32)draw_display << 10 | (u32)mask_set << 11 | (u32)mask_check << 12;
    s |= (u32)(interlace ? field : 1) << 13;
    s |= (u32)tex_disable << 15;
    s |= (u32)((dmode >> 6) & 1) << 16 | (u32)(dmode & 3) << 17 | (u32)((dmode >> 2) & 1) << 19;
    s |= (u32)((dmode >> 3) & 1) << 20 | (u32)((dmode >> 4) & 1) << 21 | (u32)interlace << 22;
    s |= (u32)disp_off << 23 | (u32)gpu_irq << 24;
    s |= 1u << 26 | (u32)(down.n > 0) << 27 | 1u << 28;
    s |= (u32)dma_dir << 29;
    const u32 req = dma_dir == 0 ? 0 : dma_dir == 1 ? 1 : dma_dir == 2 ? 1 : (u32)(down.n > 0);
    s |= req << 25;
    const int odd = interlace && ((dmode >> 2) & 1) ? field : (line_now < VISIBLE_LINES ? line_now & 1 : 0);
    s |= (u32)odd << 31;
    return s;
}

void gpu_line(int l) {
    line_now = l;
    if (l == 0) field ^= 1;
}

int gpu_frame(u32 *out, int *w, int *h) {
    static const int widths[4] = { 256, 320, 512, 640 };
    int width = dmode & 0x40 ? 368 : widths[dmode & 3];
    const int interlace = (dmode & 0x24) == 0x24;
    int lines = v2 - v1;
    if (lines <= 0 || lines > 256) lines = 240;
    if (interlace) lines *= 2;
    if (lines > 480) lines = 480;
    *w = width; *h = lines;
    if (disp_off) { memset(out, 0, (size_t)width * lines * 4); for (int i = 0; i < width * lines; i++) out[i] = 0xff000000; return 0; }
    for (int y = 0; y < lines; y++) {
        const int vy = (disp_y + y) & 511;
        u32 *o = out + y * width;
        if (dmode & 0x10) {         /* 24-bit pixels, for movies */
            const u8 *row = (const u8 *)&vram[vy * 1024];
            for (int x = 0; x < width; x++) {
                const int b = (disp_x * 2 + x * 3) & 2047;
                const u8 r = row[b], g = row[(b + 1) & 2047], bl = row[(b + 2) & 2047];
                o[x] = r | g << 8 | bl << 16 | 0xff000000u;
            }
        } else {
            for (int x = 0; x < width; x++) {
                const u16 p = vram[vy * 1024 + ((disp_x + x) & 1023)];
                const u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                o[x] = (r | r >> 5) | (g | g >> 5) << 8 | (b | b >> 5) << 16 | 0xff000000u;
            }
        }
    }
    return 1;
}

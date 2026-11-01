/* test_rdp.c - standalone tests and a benchmark for rdp.c. Pass "bench" to time it. */
#include "n64.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

u8 n64_rdram[N64_RDRAM_SIZE] N64_STATE;
u8 n64_sp_mem[0x2000] N64_STATE;
static int full_syncs;
void n64_rdp_full_sync(void) { full_syncs++; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CMDS 0x700000u
#define FB   0x100000u
#define ZB   0x200000u
#define TEX  0x300000u
#define W 320
#define H 240

static u64 list[200000];
static int nlist;
static void C(u64 v) { list[nlist++] = v; }
static void flush(void) {
    for (int i = 0; i < nlist; i++)
        for (int b = 0; b < 8; b++) n64_rdram[CMDS + (u32)i * 8 + (u32)b] = (u8)(list[i] >> (56 - b * 8));
    rdp_run(CMDS, CMDS + (u32)nlist * 8, 0);
    nlist = 0;
}
#define OP(o) ((u64)(o) << 56)
static u64 color_image(int size, u32 a) { return OP(0x3F) | (u64)size << 51 | (u64)(W - 1) << 32 | a; }
static u64 rect36(int x0, int y0, int x1, int y1) {
    return OP(0x36) | (u64)(x1 * 4) << 44 | (u64)(y1 * 4) << 32 | (u64)(x0 * 4) << 12 | (u64)(y0 * 4);
}
static u64 scissor(void) { return OP(0x2D) | (u64)(W * 4) << 12 | (H * 4); }

/* other modes */
#define CYC(n) ((u64)(n) << 52)
#define PERSP (1ull << 51)
#define BILERP (1ull << 45)
#define NODITH (3ull << 38 | 3ull << 36)
#define ZCMP 0x10u
#define ZUPD 0x20u
#define AA 0x08u
#define IMRD 0x40u
#define ALPHACVG 0x2000u
/* combine (d=TEX*SHADE style) both cycles (a-b)*c+d; same for rgb and alpha */
static u64 combine(int a, int b, int c, int d, int aa, int ab, int ac, int ad) {
    return OP(0x3C) | (u64)a << 52 | (u64)c << 47 | (u64)aa << 44 | (u64)ac << 41 | (u64)a << 37 | (u64)c << 32 |
           (u64)b << 28 | (u64)b << 24 | (u64)aa << 21 | (u64)ac << 18 | (u64)d << 15 | (u64)ab << 12 |
           (u64)ad << 9 | (u64)d << 6 | (u64)ab << 3 | (u64)ad;
}

static u32 px16(int x, int y) { u32 a = FB + (u32)(y * W + x) * 2; return (u32)n64_rdram[a] << 8 | n64_rdram[a + 1]; }

typedef struct { double x, y, r, g, b, a, s, t, w, z; } V;

static void put(u64 *ints, u64 *fracs, int lane, double v) {
    s32 f = (s32)floor(v * 65536.0 + 0.5);
    int sh = 48 - lane * 16;
    *ints |= (u64)(((u32)f >> 16) & 0xFFFF) << sh;
    *fracs |= (u64)((u32)f & 0xFFFF) << sh;
}
static u32 fx(double v) { return (u32)(s32)floor(v * 65536.0 + 0.5); }

/* Builds a real edge-coefficient triangle from three vertices. */
static void tri(int shade, int tex, int zb, const V *in, int tile) {
    V a = in[0], b = in[1], c = in[2], t;
    if (a.y > b.y) { t = a; a = b; b = t; }
    if (b.y > c.y) { t = b; b = c; c = t; }
    if (a.y > b.y) { t = a; a = b; b = t; }
    double det = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (fabs(det) < 1e-6) return;
    /* H is the a-c edge: on the left (flip=1) when b lies to its right */
    double ya = a.y, yfl = floor(ya);
    double dxh = (c.y > a.y) ? (c.x - a.x) / (c.y - a.y) : 0;
    double dxm = (b.y > a.y) ? (b.x - a.x) / (b.y - a.y) : 0;
    double dxl = (c.y > b.y) ? (c.x - b.x) / (c.y - b.y) : 0;
    double xh = a.x + (yfl - ya) * dxh, xm = a.x + (yfl - ya) * dxm;
    int yh = (int)floor(a.y * 4), ym = (int)floor(b.y * 4), yl = (int)floor(c.y * 4);
    int op = 8 | (shade ? 4 : 0) | (tex ? 2 : 0) | (zb ? 1 : 0);
    C(OP(op) | (u64)(det < 0 ? 0 : 1) << 55 | (u64)tile << 48 | (u64)(yl & 0x3FFF) << 32 | (u64)(ym & 0x3FFF) << 16 | (u64)(yh & 0x3FFF));
    C((u64)fx(b.x) << 32 | fx(dxl));
    C((u64)fx(xh) << 32 | fx(dxh));
    C((u64)fx(xm) << 32 | fx(dxm));
#define GRAD(F, dx, dy) double dx = (((b.F - a.F) * (c.y - a.y) - (c.F - a.F) * (b.y - a.y)) / det), \
        dy = (((c.F - a.F) * (b.x - a.x) - (b.F - a.F) * (c.x - a.x)) / det)
    /* attributes are specified on the major edge at (xh, yfl) */
    double ex = xh - a.x, ey = yfl - a.y;
    if (shade) {
        GRAD(r, rx, ry); GRAD(g, gx, gy); GRAD(b, bx, by); GRAD(a, ax, ay);
        double v[4] = {a.r + ex * rx + ey * ry, a.g + ex * gx + ey * gy, a.b + ex * bx + ey * by, a.a + ex * ax + ey * ay};
        double dx[4] = {rx, gx, bx, ax}, dy[4] = {ry, gy, by, ay};
        u64 w[8] = {0};
        for (int i = 0; i < 4; i++) {
            put(&w[0], &w[2], i, v[i]);
            put(&w[1], &w[3], i, dx[i]);
            put(&w[4], &w[6], i, dy[i] + dxh * dx[i]);
            put(&w[5], &w[7], i, dy[i]);
        }
        for (int i = 0; i < 8; i++) C(w[i]);
    }
    if (tex) {
        GRAD(s, sx_, sy_); GRAD(t, tx, ty); GRAD(w, wx, wy);
        double v[3] = {a.s + ex * sx_ + ey * sy_, a.t + ex * tx + ey * ty, a.w + ex * wx + ey * wy};
        double dx[3] = {sx_, tx, wx}, dy[3] = {sy_, ty, wy};
        u64 w[8] = {0};
        for (int i = 0; i < 3; i++) {
            put(&w[0], &w[2], i, v[i]);
            put(&w[1], &w[3], i, dx[i]);
            put(&w[4], &w[6], i, dy[i] + dxh * dx[i]);
            put(&w[5], &w[7], i, dy[i]);
        }
        for (int i = 0; i < 8; i++) C(w[i]);
    }
    if (zb) {
        GRAD(z, zx, zy);
        C((u64)fx(a.z + ex * zx + ey * zy) << 32 | fx(zx));
        C((u64)fx(zy + dxh * zx) << 32 | fx(zy));
    }
}

static void setup(void) {
    C(color_image(2, FB));
    C(OP(0x3E) | ZB);
    C(scissor());
}

static void test_fill(void) {
    setup();
    C(OP(0x2F) | CYC(3));
    C(OP(0x37) | 0xF801F801u);
    C(rect36(10, 20, 19, 29));
    flush();
    CHECK(px16(10, 20) == 0xF801 && px16(19, 29) == 0xF801, "fill16 inside %04x", px16(19, 29));
    CHECK(px16(20, 20) == 0 && px16(10, 30) == 0, "fill16 outside");
    /* per-pixel alternating halves */
    C(OP(0x37) | 0x12344321u);
    C(rect36(0, 0, 3, 0));
    flush();
    CHECK(px16(0, 0) == 0x1234 && px16(1, 0) == 0x4321 && px16(2, 0) == 0x1234, "fill16 pattern");
    /* 32-bit, R G B A byte order */
    C(color_image(3, FB + 0x40000));
    C(OP(0x37) | 0x11223344u);
    C(rect36(2, 1, 4, 2));
    flush();
    const u8 *p = &n64_rdram[FB + 0x40000 + (1 * W + 2) * 4];
    CHECK(p[0] == 0x11 && p[1] == 0x22 && p[2] == 0x33 && p[3] == 0x44, "fill32");
    CHECK(p[12] == 0 && n64_rdram[FB + 0x40000 + (3 * W + 2) * 4] == 0, "fill32 outside");
}

static void clear(u32 addr, u32 v) {
    C(color_image(2, addr));
    C(OP(0x2F) | CYC(3));
    C(OP(0x37) | v);
    C(rect36(0, 0, W - 1, H - 1));
    C(color_image(2, FB));
}

static void test_shaded(void) {
    setup();
    clear(FB, 0);
    C(OP(0x2F) | CYC(0) | NODITH);
    C(combine(8, 8, 16, 4, 7, 7, 7, 4));          /* shade */
    V v[3] = {{.x = 50, .y = 50, .r = 255, .g = 0, .b = 0, .a = 255},
              {.x = 150, .y = 60, .r = 255, .g = 0, .b = 0, .a = 255},
              {.x = 60, .y = 120, .r = 255, .g = 0, .b = 0, .a = 255}};
    tri(1, 0, 0, v, 0);
    flush();
    CHECK(px16(80, 70) == 0xF801, "tri interior %04x", px16(80, 70));
    CHECK(px16(40, 70) == 0 && px16(150, 110) == 0 && px16(80, 45) == 0, "tri outside");
    /* same triangle reflected so the major edge is on the other side */
    clear(FB, 0);
    C(OP(0x2F) | CYC(0) | NODITH);
    V m[3] = {{.x = 150, .y = 50, .g = 255, .a = 255}, {.x = 50, .y = 60, .g = 255, .a = 255}, {.x = 140, .y = 120, .g = 255, .a = 255}};
    tri(1, 0, 0, m, 0);
    flush();
    CHECK((px16(120, 70) & 0xFFFE) == 0x07C0, "tri right-major interior %04x", px16(120, 70));
    CHECK(px16(160, 70) == 0, "tri right-major outside");
    /* gouraud gradient */
    clear(FB, 0);
    C(OP(0x2F) | CYC(0) | NODITH);
    V g[3] = {{.x = 0, .y = 0, .r = 0, .a = 255}, {.x = 200, .y = 0, .r = 200, .a = 255}, {.x = 0, .y = 200, .r = 0, .a = 255}};
    tri(1, 0, 0, g, 0);
    flush();
    int r = (int)(px16(50, 10) >> 11) << 3;
    CHECK(r >= 40 && r <= 56, "gradient r=%d", r);
}

static void load_tex16(void) {
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        u32 a = TEX + (u32)(y * 8 + x) * 2;
        u32 c = (u32)x << 11 | (u32)y << 6 | 1;
        if (x == 7 && y == 7) c = 0xF800;       /* transparent */
        n64_rdram[a] = (u8)(c >> 8); n64_rdram[a + 1] = (u8)c;
    }
    C(OP(0x3D) | 0ull << 53 | 2ull << 51 | 7ull << 32 | TEX);
    C(OP(0x35) | 0ull << 53 | 2ull << 51 | 0ull << 41 | 0ull << 32 | 7ull << 24);
    C(OP(0x33) | 7ull << 24 | 63ull << 12 | 0x800);
    C(OP(0x35) | 2ull << 51 | 2ull << 41);
    C(OP(0x32) | 28ull << 12 | 28ull);
}

static void test_copy(void) {
    setup();
    clear(FB, 0);
    load_tex16();
    C(OP(0x2F) | CYC(2) | 1u);                    /* copy + alpha compare */
    /* textured rect at (100,100)-(107,107), S=T=0, dsdx=4.0, dtdy=1.0 */
    C(OP(0x24) | (u64)(107 * 4) << 44 | (u64)(107 * 4) << 32 | (u64)(100 * 4) << 12 | (100 * 4));
    C((u64)(4 << 10) << 16 | (1 << 10));
    flush();
    CHECK(px16(100, 100) == 1, "copy (0,0) %04x", px16(100, 100));
    CHECK(px16(103, 105) == ((3u << 11) | (5u << 6) | 1), "copy (3,5) %04x", px16(103, 105));
    CHECK(px16(107, 107) == 0, "copy transparent %04x", px16(107, 107));
    CHECK(px16(108, 100) == 0 && px16(100, 108) == 0, "copy outside");
}

static void tile_rect(int x, int y, int w, int h) {     /* 1:1 rect with S=T=0 */
    C(OP(0x24) | (u64)((x + w) * 4) << 44 | (u64)((y + h) * 4) << 32 | (u64)(x * 4) << 12 | (u64)(y * 4));
    C((u64)(1 << 10) << 16 | (1 << 10));
}
static void poke(u32 a, const u8 *d, int n) { memcpy(&n64_rdram[a], d, (size_t)n); }

static void test_formats(void) {
    setup();
    clear(FB, 0);
    u64 om = OP(0x2F) | CYC(0) | NODITH, tex0 = combine(8, 8, 16, 1, 7, 7, 7, 1);
    /* RGBA16 through a triangle with perspective (constant W=0.5) */
    load_tex16();
    C(OP(0x2F) | CYC(0) | NODITH | PERSP);
    C(tex0);
    V v[3] = {{.x = 0, .y = 0, .s = 0, .t = 0, .w = 16384}, {.x = 7.9, .y = 0, .s = 7.9 * 16, .t = 0, .w = 16384},
              {.x = 0, .y = 7.9, .s = 0, .t = 7.9 * 16, .w = 16384}};
    tri(0, 1, 0, v, 0);
    flush();
    CHECK((px16(3, 2) & 0xFFFE) == ((3u << 11) | (2u << 6)), "persp texel %04x", px16(3, 2));

    /* RGBA32: load block must split the texel across the TMEM banks */
    static const u8 t32[16] = {0xF8,0,0,0xFF, 0,0xF8,0,0xFF, 0,0,0xF8,0xFF, 0xF8,0xF8,0xF8,0xFF};
    poke(TEX + 0x1000, t32, 16);
    C(OP(0x3D) | 3ull << 51 | 3ull << 32 | (TEX + 0x1000));
    C(OP(0x35) | 3ull << 51 | 7ull << 24);
    C(OP(0x33) | 7ull << 24 | 3ull << 12 | 0x800);
    C(OP(0x35) | 3ull << 51 | 1ull << 41);
    C(OP(0x32) | 12ull << 12 | 0ull);
    C(om); C(tex0);
    tile_rect(20, 0, 4, 1);
    flush();
    CHECK((px16(20, 0) & 0xFFFE) == 0xF800 && (px16(21, 0) & 0xFFFE) == 0x07C0 && (px16(22, 0) & 0xFFFE) == 0x003E, "rgba32 %04x %04x", px16(20, 0), px16(21, 0));

    /* CI8 with a TLUT at 0x800 */
    static const u8 pal[8] = {0, 0, 0xF8, 0x01, 0x07, 0xC1, 0, 0x3F}, ci[4] = {1, 2, 3, 1};
    poke(TEX + 0x1100, pal, 8);
    poke(TEX + 0x1200, ci, 4);
    C(OP(0x3D) | 2ull << 51 | (TEX + 0x1100));
    C(OP(0x35) | 0x100ull << 32 | 7ull << 24);
    C(OP(0x30) | 7ull << 24 | 12ull << 12);
    C(OP(0x3D) | 2ull << 53 | 1ull << 51 | 3ull << 32 | (TEX + 0x1200));
    C(OP(0x35) | 2ull << 53 | 1ull << 51 | 1ull << 41 | 7ull << 24);
    C(OP(0x34) | 7ull << 24 | 12ull << 12);
    C(OP(0x35) | 2ull << 53 | 1ull << 51 | 1ull << 41);
    C(OP(0x32) | 12ull << 12 | 0ull);
    C(OP(0x2F) | CYC(0) | NODITH | 1ull << 47); C(tex0);
    tile_rect(30, 0, 4, 1);
    flush();
    CHECK(px16(30, 0) == 0xF801 && px16(31, 0) == 0x07C1 && px16(32, 0) == 0x003F, "ci8 %04x %04x %04x", px16(30, 0), px16(31, 0), px16(32, 0));

    /* IA8 alpha reaches the combiner: TEX alpha over a threshold via alpha compare */
    static const u8 ia[2] = {0xFF, 0xF0};   /* I=15 A=15, I=15 A=0 */
    poke(TEX + 0x1300, ia, 2);
    C(OP(0x3D) | 3ull << 53 | 1ull << 51 | 1ull << 32 | (TEX + 0x1300));
    C(OP(0x35) | 3ull << 53 | 1ull << 51 | 1ull << 41 | 7ull << 24);
    C(OP(0x34) | 7ull << 24 | 4ull << 12);
    C(OP(0x35) | 3ull << 53 | 1ull << 51 | 1ull << 41);
    C(OP(0x32) | 4ull << 12 | 0ull);
    C(OP(0x39) | 0x80u);
    C(OP(0x2F) | CYC(0) | NODITH | 1u);
    C(tex0);
    tile_rect(40, 0, 2, 1);
    flush();
    CHECK((px16(40, 0) & 0xFFFE) == 0xFFFE && px16(41, 0) == 0, "ia8 %04x %04x", px16(40, 0), px16(41, 0));

    /* wrap, mirror and clamp, 2 texel wide RGBA16 texture */
    static const u8 t2[4] = {0xF8, 0x01, 0x00, 0x3F};
    poke(TEX + 0x1400, t2, 4);
    C(OP(0x3D) | 2ull << 51 | 1ull << 32 | (TEX + 0x1400));
    C(OP(0x35) | 2ull << 51 | 7ull << 24);
    C(OP(0x33) | 7ull << 24 | 1ull << 12 | 0x800);
    C(OP(0x35) | 2ull << 51 | 1ull << 41 | 1ull << 8 | 1ull << 4);     /* mirror, mask 1 */
    C(OP(0x32) | 4ull << 12 | 0ull);
    C(om); C(tex0);
    tile_rect(50, 0, 4, 1);
    flush();
    CHECK(px16(50, 0) == 0xF801 && px16(51, 0) == 0x003F && px16(52, 0) == 0x003F && px16(53, 0) == 0xF801,
          "mirror %04x %04x %04x %04x", px16(50, 0), px16(51, 0), px16(52, 0), px16(53, 0));
    C(OP(0x35) | 2ull << 51 | 1ull << 41);                             /* no mask: clamp */
    C(OP(0x32) | 4ull << 12 | 0ull);
    C(om); C(tex0);
    tile_rect(60, 0, 4, 1);
    flush();
    CHECK(px16(60, 0) == 0xF801 && px16(61, 0) == 0x003F && px16(63, 0) == 0x003F, "clamp %04x", px16(63, 0));

    /* bilinear: halfway between red and blue at S=0.5 */
    C(OP(0x2F) | CYC(0) | NODITH | BILERP);
    C(tex0);
    C(OP(0x24) | (u64)(71 * 4) << 44 | (u64)(1 * 4) << 32 | (u64)(70 * 4) << 12 | 0);
    C((u64)16 << 48 | (u64)(1 << 10) << 16 | (1 << 10));
    flush();
    u32 p = px16(70, 0);
    CHECK(((p >> 11) & 31) >= 14 && ((p >> 11) & 31) <= 16 && ((p >> 1) & 31) >= 14 && ((p >> 1) & 31) <= 16, "bilinear %04x", p);
}

static void test_z_format(void) {
    /* opaque write stores the compressed value: prim Z 0, dz 1 */
    setup();
    clear(ZB, 0xFFFCFFFC);
    C(OP(0x2E) | 0x7000u << 16 | 1u);
    C(OP(0x2F) | CYC(0) | NODITH | ZCMP | ZUPD | 4u);
    C(combine(8, 8, 16, 3, 7, 7, 7, 3));
    C(rect36(0, 0, 2, 1));
    flush();
    u32 v = (u32)n64_rdram[ZB] << 8 | n64_rdram[ZB + 1];
    /* 0x7000<<3 = 0x38000: exponent 3, mantissa 0 */
    CHECK(v == (3u << 13), "z format %04x", v);
}

static void test_z(void) {
    setup();
    clear(ZB, 0xFFFCFFFC);
    clear(FB, 0);
    C(OP(0x2F) | CYC(0) | NODITH | ZCMP | ZUPD);
    C(combine(8, 8, 16, 4, 7, 7, 7, 4));
    /* near red over far blue, far drawn second must not win */
    V n[3] = {{.x = 20, .y = 20, .r = 255, .a = 255, .z = 0x2000}, {.x = 120, .y = 20, .r = 255, .a = 255, .z = 0x2000},
              {.x = 20, .y = 120, .r = 255, .a = 255, .z = 0x2000}};
    V f[3] = {{.x = 10, .y = 10, .b = 255, .a = 255, .z = 0x6000}, {.x = 200, .y = 10, .b = 255, .a = 255, .z = 0x6000},
              {.x = 10, .y = 200, .b = 255, .a = 255, .z = 0x6000}};
    tri(1, 0, 1, n, 0);
    tri(1, 0, 1, f, 0);
    flush();
    CHECK((px16(40, 40) & 0xF800) == 0xF800 && !(px16(40, 40) & 0x3E), "near stays %04x", px16(40, 40));
    CHECK((px16(150, 30) & 0x3E) == 0x3E, "far fills the rest %04x", px16(150, 30));
    /* nearer one drawn after replaces */
    V g[3] = {{.x = 30, .y = 30, .g = 255, .a = 255, .z = 0x1000}, {.x = 90, .y = 30, .g = 255, .a = 255, .z = 0x1000},
              {.x = 30, .y = 90, .g = 255, .a = 255, .z = 0x1000}};
    tri(1, 0, 1, g, 0);
    flush();
    CHECK((px16(40, 40) & 0x7C0) == 0x7C0 && !(px16(40, 40) & 0xF800), "nearer wins %04x", px16(40, 40));
}

static void test_combiner(void) {
    setup();
    clear(FB, 0);
    /* (PRIM - ENV) * LOD-prim-frac + ENV: lerp with frac 0x80 */
    C(OP(0x2F) | CYC(0) | NODITH);
    C(OP(0x3A) | 0x80ull << 32 | 0xF8000000u | 0xFF);
    C(OP(0x3B) | 0x0000F8FFu);
    C(combine(3, 5, 14, 5, 7, 7, 7, 3));
    C(rect36(0, 0, 10, 10));
    flush();
    u32 p = px16(5, 5);
    int r = (int)(p >> 11) & 31, b = (int)(p >> 1) & 31;
    CHECK(r >= 15 && r <= 16 && b >= 15 && b <= 16, "lerp r=%d b=%d", r, b);

    /* blender: force blend, half blend colour (alpha 0x80) over memory */
    C(OP(0x37) | 0xF800F800u);
    clear(FB, 0xF801F801);
    C(OP(0x2F) | CYC(0) | NODITH | IMRD | 0x4000u | (2u << 30) | (3u << 26 >> 0) * 0 | (0u << 26) | (1u << 22) | (0u << 18));
    C(OP(0x39) | 0x0000F800u);
    C(OP(0x3A) | 0x0000F880u);
    C(combine(8, 8, 16, 3, 7, 7, 7, 3));
    C(rect36(0, 0, 8, 8));
    flush();
    p = px16(3, 3);
    r = (int)(p >> 11) & 31;
    b = (int)(p >> 1) & 31;
    CHECK(r >= 14 && r <= 17 && b >= 14 && b <= 17, "blend r=%d b=%d (%04x)", r, b, p);
}

static void test_sync_and_partial(void) {
    setup();
    flush();
    int before = full_syncs;
    /* a triangle split over two rdp_run calls, fed through DMEM */
    V v[3] = {{.x = 5, .y = 5, .r = 255, .a = 255}, {.x = 40, .y = 5, .r = 255, .a = 255}, {.x = 5, .y = 40, .r = 255, .a = 255}};
    clear(FB, 0);
    C(OP(0x2F) | CYC(0) | NODITH);
    C(combine(8, 8, 16, 4, 7, 7, 7, 4));
    flush();
    tri(1, 0, 0, v, 0);
    C(OP(0x29));
    for (int i = 0; i < nlist; i++)
        for (int b = 0; b < 8; b++) n64_sp_mem[(u32)i * 8 + (u32)b] = (u8)(list[i] >> (56 - b * 8));
    int bytes = nlist * 8;
    nlist = 0;
    rdp_run(0, 48, 1);
    CHECK(px16(10, 10) == 0, "partial command must wait");
    rdp_run(48, (u32)bytes, 1);
    CHECK((px16(10, 10) & 0xF800) == 0xF800, "resumed %04x", px16(10, 10));
    CHECK(full_syncs == before + 1, "sync full");
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* Worst typical SM64 surface: perspective, bilinear, MODULATE, Z, AA. */
static void bench_case(const char *name, u64 om, u64 cc) {
    setup();
    load_tex16();
    C(OP(0x35) | 2ull << 51 | 2ull << 41 | 3ull << 14 | 3ull << 4);
    flush();
    double total = 0;
    int pixels = 0;
    const int frames = 100;
    for (int f = 0; f < frames; f++) {
        clear(ZB, 0xFFFCFFFC);
        clear(FB, 0x00010001);
        C(om);
        C(cc);
        for (int i = 0; i < 400; i++) {
            double x = (i * 37) % 280, y = (i * 53) % 200, sz = 24;
            double w0 = 0.7, w1 = 0.5;
            V v[3] = {{.x = x, .y = y, .r = 200, .g = 180, .b = 160, .a = 255, .s = 0, .t = 0, .w = w0 * 32767, .z = 0x4000},
                      {.x = x + sz * 1.7, .y = y + 3, .r = 90, .g = 180, .b = 255, .a = 255, .s = 640 * w1, .t = 0, .w = w1 * 32767, .z = 0x4100},
                      {.x = x + 2, .y = y + sz * 1.7, .r = 255, .g = 80, .b = 60, .a = 255, .s = 0, .t = 640 * w1, .w = w1 * 32767, .z = 0x3F00}};
            v[0].s *= w0; v[0].t *= w0;
            tri(1, 1, 1, v, 0);
            pixels += (int)(0.5 * sz * sz * 1.7 * 1.7);
        }
        int n = nlist;
        for (int i = 0; i < n; i++)
            for (int b = 0; b < 8; b++) n64_rdram[CMDS + (u32)i * 8 + (u32)b] = (u8)(list[i] >> (56 - b * 8));
        nlist = 0;
        double t0 = now_ms();
        rdp_run(CMDS, CMDS + (u32)n * 8, 0);
        total += now_ms() - t0;
    }
    printf("%-34s %7.3f ms/frame  %5.1f ns/pixel (%d est. pixels/frame)\n",
           name, total / frames, total * 1e6 / pixels, pixels / frames);
}

int main(int argc, char **argv) {
    rdp_reset();
    test_fill();
    test_shaded();
    test_copy();
    test_formats();
    test_z();
    test_z_format();
    test_combiner();
    test_sync_and_partial();
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        u64 tex_x_shade = combine(1, 8, 4, 7, 7, 7, 7, 4);
        bench_case("1cyc persp bilerp Z AA", OP(0x2F) | CYC(0) | PERSP | BILERP | ZCMP | ZUPD | AA | IMRD | ALPHACVG |
                   (0u << 30) | (0u << 26) | (1u << 22) | (1u << 18), tex_x_shade);
        bench_case("2cyc fog persp bilerp Z AA", OP(0x2F) | CYC(1) | PERSP | BILERP | ZCMP | ZUPD | AA | IMRD | ALPHACVG |
                   (3u << 30) | (0u << 28) | (2u << 26) | (0u << 24) | (0u << 22) | (1u << 20) | (0u << 18) | (1u << 16), tex_x_shade);
        bench_case("1cyc point Z", OP(0x2F) | CYC(0) | PERSP | ZCMP | ZUPD, tex_x_shade);
    }
    printf(fails ? "FAILED: %d\n" : "all RDP tests passed\n", fails);
    return fails != 0;
}

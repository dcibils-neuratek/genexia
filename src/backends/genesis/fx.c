/*
 * fx.c - combo auras drawn onto the finished frame.
 *
 * The VDP marks every output pixel where P1's (palette 2) or P2's
 * (palette 3) fighter sprite won compositing (vdp_fx_mask), so effects
 * follow the fighter's exact silhouette in every animation frame.
 *
 *   fire    flame tongues rising from the silhouette (column distance field
 *           modulated by upward-scrolling noise), warm rim light, embers
 *   thunder blue glow hugging the outline (chamfer distance field), electric
 *           tint on the body, jagged lightning bolts arcing outward
 * Both start with a short screen flash and fade out after the combo ends.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "combo.h"

#define MAXW VDP_FB_STRIDE
#define MAXH 240

static u8 dist[MAXH][MAXW];        /* chamfer distance to the silhouette (capped) */
static u8 rise[MAXH][MAXW];        /* pixels above the nearest silhouette pixel below */
static u32 rng = 0x9E3779B9u;
static inline u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static inline float frand(void) { return (rnd() & 0xFFFFFF) / 16777216.0f; }

static inline float hash2(int x, int y) {
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static float noise(float x, float y) {          /* smooth value noise */
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static inline void add_px(u32 *p, int r, int g, int b) {
    int R = (int)((*p >> 16) & 255) + r, G = (int)((*p >> 8) & 255) + g, B = (int)(*p & 255) + b;
    *p = (u32)((R > 255 ? 255 : R) << 16 | (G > 255 ? 255 : G) << 8 | (B > 255 ? 255 : B));
}
static inline void tint_px(u32 *p, int r, int g, int b, float k) {
    int R = (int)((*p >> 16) & 255), G = (int)((*p >> 8) & 255), B = (int)(*p & 255);
    R += (int)((r - R) * k); G += (int)((g - G) * k); B += (int)((b - B) * k);
    *p = (u32)(R << 16 | G << 8 | B);
}

typedef struct { float x, y, vx, vy; int life, max; } Ember;
static Ember embers[2][96];
typedef struct { int n; s16 x[24], y[24]; int life; } Bolt;
static Bolt bolts[2][10];

/* bounding box of the player's silhouette; returns 0 if not on screen */
static int bbox(int who, int w, int h, int stride, int *x0, int *y0, int *x1, int *y1) {
    *x0 = w; *y0 = h; *x1 = -1; *y1 = -1;
    for (int y = 0; y < h; y++) {
        const u8 *m = &vdp_fx_mask[y * stride];
        for (int x = 0; x < w; x++) if (m[x] == who) {
            if (x < *x0) *x0 = x; if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y; if (y > *y1) *y1 = y;
        }
    }
    return *x1 >= 0;
}

/* chamfer (3-4) distance transform inside the box, in pixel units */
static void distance_field(int who, int stride, int X0, int Y0, int X1, int Y1) {
    for (int y = Y0; y <= Y1; y++)
        for (int x = X0; x <= X1; x++) dist[y][x] = vdp_fx_mask[y * stride + x] == who ? 0 : 255;
    for (int y = Y0; y <= Y1; y++)
        for (int x = X0; x <= X1; x++) {
            int d = dist[y][x];
            if (x > X0 && dist[y][x - 1] + 3 < d) d = dist[y][x - 1] + 3;
            if (y > Y0) {
                if (dist[y - 1][x] + 3 < d) d = dist[y - 1][x] + 3;
                if (x > X0 && dist[y - 1][x - 1] + 4 < d) d = dist[y - 1][x - 1] + 4;
                if (x < X1 && dist[y - 1][x + 1] + 4 < d) d = dist[y - 1][x + 1] + 4;
            }
            dist[y][x] = (u8)(d > 255 ? 255 : d);
        }
    for (int y = Y1; y >= Y0; y--)
        for (int x = X1; x >= X0; x--) {
            int d = dist[y][x];
            if (x < X1 && dist[y][x + 1] + 3 < d) d = dist[y][x + 1] + 3;
            if (y < Y1) {
                if (dist[y + 1][x] + 3 < d) d = dist[y + 1][x] + 3;
                if (x < X1 && dist[y + 1][x + 1] + 4 < d) d = dist[y + 1][x + 1] + 4;
                if (x > X0 && dist[y + 1][x - 1] + 4 < d) d = dist[y + 1][x - 1] + 4;
            }
            dist[y][x] = (u8)(d > 255 ? 255 : d);
        }
}

static void fire(int p, u32 *fb, int stride, int w, int h, float level, int t) {
    int who = p + 1, bx0, by0, bx1, by1;
    if (!bbox(who, w, h, stride, &bx0, &by0, &bx1, &by1)) return;
    const int R = 34;                                   /* flame height */
    int X0 = bx0 - 10 < 0 ? 0 : bx0 - 10, X1 = bx1 + 10 >= w ? w - 1 : bx1 + 10;
    int Y0 = by0 - R < 0 ? 0 : by0 - R, Y1 = by1;
    distance_field(who, stride, X0, Y0, X1, Y1);
    for (int x = X0; x <= X1; x++) {                    /* rise: distance upward from the body */
        int d = 255;
        for (int y = Y1; y >= Y0; y--) {
            if (vdp_fx_mask[y * stride + x] == who) d = 0; else if (d < 255) d++;
            rise[y][x] = (u8)d;
        }
    }
    float ts = t * 0.22f;
    for (int y = Y0; y <= Y1; y++) {
        u32 *row = &fb[y * stride];
        for (int x = X0; x <= X1; x++) {
            int dd = dist[y][x];
            if (dd == 0) {
                /* warm rim light on the body near its edge */
                int edge = 0;
                for (int k = 1; k <= 2 && !edge; k++)
                    edge = (x - k >= 0 && vdp_fx_mask[y * stride + x - k] != who) ||
                           (x + k < w && vdp_fx_mask[y * stride + x + k] != who) ||
                           (y - k >= 0 && vdp_fx_mask[(y - k) * stride + x] != who);
                if (edge) add_px(&row[x], (int)(150 * level), (int)(70 * level), (int)(10 * level));
                else tint_px(&row[x], 255, 130, 40, 0.16f * level);
                continue;
            }
            float up = rise[y][x] < R ? 1.0f - rise[y][x] / (float)R : 0.0f;   /* tongues above */
            float side = dd < 21 ? 1.0f - dd / 21.0f : 0.0f;                   /* thin glow around */
            float n = noise(x * 0.20f, y * 0.11f + ts) * 0.65f + noise(x * 0.45f, y * 0.3f + ts * 2.1f) * 0.35f;
            float heat = up * (0.45f + 1.25f * n) - 0.22f;
            float glow = side * side * 0.8f;
            if (heat < glow) heat = glow;
            if (heat <= 0) continue;
            if (heat > 1) heat = 1;
            int r, g, b;
            if (heat < 0.35f)      { r = (int)(heat / 0.35f * 190); g = (int)(heat / 0.35f * 30); b = 0; }
            else if (heat < 0.7f)  { float k = (heat - 0.35f) / 0.35f; r = 190 + (int)(65 * k); g = 30 + (int)(130 * k); b = 0; }
            else                   { float k = (heat - 0.7f) / 0.3f; r = 255; g = 160 + (int)(90 * k); b = (int)(170 * k); }
            float a = level * (heat * 1.5f > 1 ? 1 : heat * 1.5f);
            add_px(&row[x], (int)(r * a), (int)(g * a), (int)(b * a));
        }
    }
    /* embers: spawned on top edges of the silhouette, drift upward */
    for (int s = 0; s < 5; s++) {
        int x = bx0 + (int)(rnd() % (u32)(bx1 - bx0 + 1));
        for (int y = by0; y <= by1; y++)
            if (vdp_fx_mask[y * stride + x] == who) {
                for (int i = 0; i < 96; i++) if (embers[p][i].life <= 0) {
                    embers[p][i] = (Ember){ (float)x, (float)y, (frand() - 0.5f) * 0.6f, -0.7f - frand() * 1.1f, 0, 0 };
                    embers[p][i].max = embers[p][i].life = 18 + (int)(rnd() % 26);
                    break;
                }
                break;
            }
    }
    for (int i = 0; i < 96; i++) {
        Ember *e = &embers[p][i];
        if (e->life <= 0) continue;
        e->x += e->vx + (noise(e->y * 0.1f, t * 0.05f) - 0.5f) * 0.8f; e->y += e->vy; e->life--;
        int xi = (int)e->x, yi = (int)e->y;
        if (xi < 0 || yi < 0 || xi >= w || yi >= h) { e->life = 0; continue; }
        float a = level * e->life / (float)e->max;
        add_px(&fb[yi * stride + xi], (int)(255 * a), (int)(190 * a), (int)(60 * a));
        if (xi + 1 < w) add_px(&fb[yi * stride + xi + 1], (int)(140 * a), (int)(60 * a), 0);
    }
}

static void draw_line(u32 *fb, int stride, int w, int h, int x0, int y0, int x1, int y1, float a) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        if (x0 >= 1 && y0 >= 1 && x0 < w - 1 && y0 < h - 1) {
            add_px(&fb[y0 * stride + x0], (int)(255 * a), (int)(255 * a), (int)(255 * a));
            add_px(&fb[y0 * stride + x0 + 1], (int)(60 * a), (int)(120 * a), (int)(255 * a));
            add_px(&fb[y0 * stride + x0 - 1], (int)(60 * a), (int)(120 * a), (int)(255 * a));
            add_px(&fb[(y0 + 1) * stride + x0], (int)(40 * a), (int)(90 * a), (int)(230 * a));
            add_px(&fb[(y0 - 1) * stride + x0], (int)(40 * a), (int)(90 * a), (int)(230 * a));
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void thunder(int p, u32 *fb, int stride, int w, int h, float level, int t) {
    int who = p + 1, bx0, by0, bx1, by1;
    if (!bbox(who, w, h, stride, &bx0, &by0, &bx1, &by1)) return;
    const int G = 9;
    int X0 = bx0 - G < 0 ? 0 : bx0 - G, X1 = bx1 + G >= w ? w - 1 : bx1 + G;
    int Y0 = by0 - G < 0 ? 0 : by0 - G, Y1 = by1 + G >= h ? h - 1 : by1 + G;
    distance_field(who, stride, X0, Y0, X1, Y1);
    float flick = 0.7f + 0.3f * noise(t * 0.7f, 3.0f);
    float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
    int edges = 0; s16 ex[512], ey[512];
    for (int y = Y0; y <= Y1; y++) {
        u32 *row = &fb[y * stride];
        for (int x = X0; x <= X1; x++) {
            int dd = dist[y][x];
            if (dd == 0) {
                /* electric body: cool tint plus crawling bright streaks */
                float n = noise(x * 0.35f + t * 0.9f, y * 0.35f - t * 0.6f);
                tint_px(&row[x], 150, 200, 255, 0.18f * level);
                if (n > 0.78f) add_px(&row[x], (int)(90 * level), (int)(140 * level), (int)(200 * level));
                continue;
            }
            float d = dd / 3.0f;                          /* chamfer units -> pixels */
            if (dd == 3 && edges < 512 && (rnd() & 7) == 0) { ex[edges] = (s16)x; ey[edges] = (s16)y; edges++; }
            if (d > G) continue;
            float g = (1.0f - d / G);
            g = g * g * flick * level;
            add_px(&row[x], (int)(70 * g), (int)(150 * g), (int)(255 * g));
        }
    }
    /* lightning bolts from the outline outward; each lives 3 frames */
    for (int i = 0; i < 10; i++) if (bolts[p][i].life > 0) bolts[p][i].life--;
    if (edges && (t % 2 == 0)) {
        for (int k = 0; k < 3; k++) {
            Bolt *b = NULL;
            for (int i = 0; i < 10; i++) if (bolts[p][i].life <= 0) { b = &bolts[p][i]; break; }
            if (!b) break;
            int e = (int)(rnd() % (u32)edges);
            float x = ex[e], y = ey[e];
            float dx = x - cx, dy = y - cy, l = sqrtf(dx * dx + dy * dy) + 1e-3f;
            dx /= l; dy /= l;
            int len = 5 + (int)(rnd() % 6);
            b->n = 0; b->life = 3;
            for (int s = 0; s <= len && b->n < 24; s++) {
                b->x[b->n] = (s16)x; b->y[b->n] = (s16)y; b->n++;
                float jx = (frand() - 0.5f) * 5.0f, jy = (frand() - 0.5f) * 5.0f;
                x += dx * 3.2f + jx; y += dy * 3.2f + jy;
            }
        }
    }
    for (int i = 0; i < 10; i++) {
        Bolt *b = &bolts[p][i];
        if (b->life <= 0) continue;
        float a = level * (b->life / 3.0f);
        for (int s = 1; s < b->n; s++)
            draw_line(fb, stride, w, h, b->x[s - 1], b->y[s - 1], b->x[s], b->y[s], a);
    }
}

/* ------------------------------------------------- text: 5x7 pixel font */
static const char *glyph5x7(char c) {
    static const struct { char c; const char *rows; } g[] = {
        {'A',"01110100011000111111100011000110001"},{'B',"11110100011000111110100011000111110"},
        {'C',"01110100011000010000100001000101110"},{'D',"11100100101000110001100011001011100"},
        {'E',"11111100001000011110100001000011111"},{'F',"11111100001000011110100001000010000"},
        {'G',"01110100011000010111100011000101111"},{'H',"10001100011000111111100011000110001"},
        {'I',"01110001000010000100001000010001110"},{'J',"00111000100001000010000101001001100"},
        {'K',"10001100101010011000101001001010001"},{'L',"10000100001000010000100001000011111"},
        {'M',"10001110111010110101100011000110001"},{'N',"10001100011100110101100111000110001"},
        {'O',"01110100011000110001100011000101110"},{'P',"11110100011000111110100001000010000"},
        {'Q',"01110100011000110001101011001001101"},{'R',"11110100011000111110101001001010001"},
        {'S',"01111100001000001110000010000111110"},{'T',"11111001000010000100001000010000100"},
        {'U',"10001100011000110001100011000101110"},{'V',"10001100011000110001100010101000100"},
        {'W',"10001100011000110101101011010101010"},{'X',"10001100010101000100010101000110001"},
        {'Y',"10001100010101000100001000010000100"},{'Z',"11111000010001000100010001000011111"},
        {'0',"01110100011001110101110011000101110"},{'1',"00100011000010000100001000010001110"},
        {'2',"01110100010000100010001000100011111"},{'3',"11111000100010000010000011000101110"},
        {'4',"00010001100101010010111110001000010"},{'5',"11111100001111000001000011000101110"},
        {'6',"00110010001000011110100011000101110"},{'7',"11111000010001000100010000100001000"},
        {'8',"01110100011000101110100011000101110"},{'9',"01110100011000101111000010001001100"},
        {'!',"00100001000010000100001000000000100"},
        {'%',"11001110010001000100010011001100011"},{'-',"00000000000000011111000000000000000"},
        {':',"00000011000110000000011000110000000"},{'/',"00001000010001000100010001000010000"},
        {'<',"00010001000100010000010000010000010"},{'>',"01000001000001000001000100010001000"},
        {'\'',"00100001000100000000000000000000000"},{'.',"00000000000000000000000000110001100"},
    };
    for (unsigned i = 0; i < sizeof g / sizeof *g; i++) if (g[i].c == c) return g[i].rows;
    return NULL;
}
static int text_width(const char *t, int sc) { return (int)strlen(t) * 6 * sc; }
static void draw_text(u32 *fb, int stride, int w, int h, const char *t, int x, int y, int sc,
                      u32 col, u32 col2, float a) {
    for (int pass = 0; pass < 2; pass++)              /* 0: dark outline, 1: gradient fill */
        for (int i = 0; t[i]; i++) {
            const char *g = glyph5x7(t[i] >= 'a' && t[i] <= 'z' ? t[i] - 32 : t[i]);
            if (!g) continue;
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++) if (g[r * 5 + c] == '1')
                    for (int yy = 0; yy < sc; yy++)
                        for (int xx = 0; xx < sc; xx++) {
                            int px = x + i * 6 * sc + c * sc + xx, py = y + r * sc + yy;
                            if (pass == 0) {
                                for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
                                    int qx = px + ox, qy = py + oy;
                                    if (qx >= 0 && qy >= 0 && qx < w && qy < h) tint_px(&fb[qy * stride + qx], 0, 0, 0, 0.85f * a);
                                }
                            } else if (px >= 0 && py >= 0 && px < w && py < h) {
                                u32 cc = r < 4 ? col : col2;
                                tint_px(&fb[py * stride + px], (int)((cc >> 16) & 255), (int)((cc >> 8) & 255), (int)(cc & 255), a);
                            }
                        }
        }
}

/* text and dimming for the options menu */
void fx_text(u32 *fb, int stride, int w, int h, const char *t, int x, int y, int sc, u32 c1, u32 c2) {
    draw_text(fb, stride, w, h, t, x, y, sc, c1, c2, 1.0f);
}
int fx_text_width(const char *t, int sc) { return text_width(t, sc); }
void fx_box(u32 *fb, int stride, int w, int h, int x0, int y0, int x1, int y1, u32 c, float a) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (x >= 0 && y >= 0 && x < w && y < h)
                tint_px(&fb[y * stride + x], (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255), a);
}

static void combo_hud(int p, u32 *fb, int stride, int w, int h, float level) {
    int fire_ = combo_fx_type[p] == FX_FIRE;
    u32 c1 = fire_ ? 0xFFF060 : 0xE0FFFF, c2 = fire_ ? 0xFF6010 : 0x40A0FF;
    int hud_x = (w - vdp_native_w) / 2;
    const char *name = combo_current_name(p);
    int y = 76;
    int nx = p == 0 ? hud_x + 6 : hud_x + vdp_native_w - 6 - text_width(name, 1);
    draw_text(fb, stride, w, h, name, nx, y, 1, c1, c2, level);
    if (combo_hits[p] >= 2) {
        char buf[32];
        snprintf(buf, sizeof buf, "%d HITS!", combo_hits[p]);
        int sc = combo_hit_pop[p] < 6 ? 3 : 2;           /* pops bigger on each new hit */
        int tx = p == 0 ? hud_x + 6 : hud_x + vdp_native_w - 6 - text_width(buf, sc);
        draw_text(fb, stride, w, h, buf, tx, y + 11, sc, c1, c2, level);
    }
}

static void fill_rect(u32 *fb, int stride, int w, int h, int x0, int y0, int x1, int y1, u32 c, float a) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (x >= 0 && y >= 0 && x < w && y < h)
                tint_px(&fb[y * stride + x], (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255), a);
}

/* mana meter under each fighter's name; READY + key hint when full */
static void mana_meter(int p, u32 *fb, int stride, int w, int h, int t) {
    const int BW = 70, BH = 5, Y = 56;
    int hud_x = (w - vdp_native_w) / 2;
    int x0 = p == 0 ? hud_x + 8 : hud_x + vdp_native_w - 8 - BW;
    double m = combo_mana[p];
    int full = m >= 1.0;
    fill_rect(fb, stride, w, h, x0 - 1, Y - 1, x0 + BW + 1, Y + BH + 1, 0x000000, 0.9f);
    fill_rect(fb, stride, w, h, x0, Y, x0 + BW, Y + BH, 0x202040, 0.85f);
    int fw = (int)(BW * m);
    /* P1 fills left-to-right, P2 right-to-left (mirrors the health bars) */
    int fx0 = p == 0 ? x0 : x0 + BW - fw, fx1 = fx0 + fw;
    for (int x = fx0; x < fx1; x++) {
        float k = (float)(p == 0 ? x - x0 : x0 + BW - 1 - x) / BW;
        u32 c;
        if (full) {
            float pulse = 0.5f + 0.5f * sinf(t * 0.25f + k * 6.0f);
            int r = 255, g = 190 + (int)(60 * pulse), b = 40 + (int)(170 * pulse);
            c = (u32)(r << 16 | g << 8 | b);
        } else {
            c = (u32)((int)(60 + 140 * k) << 16 | (int)(90 + 40 * k) << 8 | 255);
        }
        fill_rect(fb, stride, w, h, x, Y, x + 1, Y + BH, c, 1.0f);
    }
    if (full) {                                          /* READY inside the glowing bar */
        const char *rd = "READY";
        float blink = (t / 15) % 2 ? 1.0f : 0.75f;
        draw_text(fb, stride, w, h, rd, x0 + (BW - text_width(rd, 1)) / 2 + 1, Y - 1, 1, 0xFFFFFF, 0xFFE080, blink);
    }
    /* second line: key hint when ready, NOT READY flash when pressed too early */
    if (combo_denied[p] > 0) {
        if ((combo_denied[p] / 4) % 2 == 0) {
            const char *nr = "NOT READY";
            int tx = p == 0 ? x0 : x0 + BW - text_width(nr, 1);
            draw_text(fb, stride, w, h, nr, tx, Y + 8, 1, 0xFF9090, 0xFF2020, 1.0f);
        }
    } else if (full) {
        const char *hint = combo_pad_hint[p] ? "LB FIRE LT THUNDER"
                         : p == 0 ? "Q FIRE  W THUNDER" : "U FIRE  O THUNDER";
        int hx = p == 0 ? x0 : x0 + BW - text_width(hint, 1);
        draw_text(fb, stride, w, h, hint, hx, Y + 8, 1, 0xFFFFFF, 0xC0C0FF, 1.0f);
    }
}

void fx_apply(u32 *fb, int stride, int w, int h) {
    if (!combo_in_fight() && !combo_fx_frames[0] && !combo_fx_frames[1]) return;
    static int t;
    t++;
    if (combo_mana_enabled && combo_in_fight())
        for (int p = 0; p < 2; p++) mana_meter(p, fb, stride, w, h, t);
    for (int p = 0; p < 2; p++) {
        if (combo_flash[p] > 0) {                       /* start flash */
            float k = combo_flash[p] / 6.0f * 0.35f;
            int fire_ = combo_fx_type[p] == FX_FIRE;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    tint_px(&fb[y * stride + x], 255, fire_ ? 190 : 230, fire_ ? 120 : 255, k);
        }
        if (combo_fx_frames[p] <= 0) continue;
        float level = combo_fx_frames[p] >= 40 ? 1.0f : combo_fx_frames[p] / 40.0f;
        if (combo_fx_type[p] == FX_FIRE) fire(p, fb, stride, w, h, level, t);
        else thunder(p, fb, stride, w, h, level, t);
        combo_hud(p, fb, stride, w, h, level);
    }
}

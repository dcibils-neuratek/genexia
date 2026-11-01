/* test_ppu.c - standalone checks for ppu.c. Build:
 *   clang -O2 -Wall -Wextra -std=c11 src/snes/ppu.c src/snes/test_ppu.c -o test_ppu */
#include "snes.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int vdp_widescreen, vdp_no_sprite_limit;
u32 vdp_framebuffer[VDP_FB_STRIDE * 240];
int vdp_screen_w, vdp_screen_h, vdp_native_w, vdp_skip_render;

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long A = (long long)(a), B = (long long)(b); checks++; \
    if (A != B) { fails++; printf("FAIL line %d: %s = %llx, expected %llx\n", __LINE__, #a, A, B); } } while (0)

#define RED   0x001F
#define GREEN 0x03E0
#define BLUE  0x7C00
static void w(int reg, int v) { snes_ppu_write((u8)reg, (u8)v); }
static void w16(int reg, int v) { w(reg, v & 0xFF); w(reg, v >> 8); }
static void wp(int reg, int v) { w(reg, v & 0xFF); w(reg + 1, v >> 8); }
static u32 fb(int x, int row) { return vdp_framebuffer[row * VDP_FB_STRIDE + x]; }

static void vaddr(int a) { w(0x15, 0x80); wp(0x16, a); }
static void vword(int v) { w(0x18, v & 0xFF); w(0x19, v >> 8); }
static void color(int i, int c) { w(0x21, i); w(0x22, c & 0xFF); w(0x22, c >> 8); }

/* 4bpp tile: every row has low-plane bytes p0,p1 and high-plane bytes p2,p3 */
static void tile4(int a, int p0, int p1, int p2, int p3)
{
    vaddr(a);
    for (int i = 0; i < 8; i++) vword(p0 | p1 << 8);
    for (int i = 0; i < 8; i++) vword(p2 | p3 << 8);
}

static void frame(void)
{
    snes_ppu_frame_start();
    for (int l = 1; l <= 224; l++) snes_ppu_render_line(l);
    snes_ppu_vblank();
}

static void hide_sprites(void)
{
    wp(0x02, 0);
    for (int i = 0; i < 128; i++) { w(0x04, 0); w(0x04, 240); w(0x04, 0); w(0x04, 0); }
    for (int i = 0; i < 32; i++) w(0x04, 0);
}

/* Mode 1, BG1 map $1000 chr $2000, BG2 map $1400 chr $3000. BG tile 1 = solid
 * colour 1, tile 2 = left half colour 1, right half colour 2. */
static void setup_mode1(void)
{
    snes_ppu_reset();
    w(0x00, 0x80);
    w(0x05, 1);
    w(0x07, 0x10);
    w(0x08, 0x14);
    w(0x0B, 0x32);
    tile4(0x2010, 0xFF, 0, 0, 0);
    tile4(0x2020, 0xF0, 0x0F, 0, 0);
    tile4(0x3010, 0xFF, 0, 0, 0);
    vaddr(0x1000);
    for (int i = 0; i < 1024; i++) vword(0);
    vaddr(0x1400);
    for (int i = 0; i < 1024; i++) vword(0);
    hide_sprites();
}

static void fill_map(int base, int entry)
{
    vaddr(base);
    for (int i = 0; i < 1024; i++) vword(entry);
}

static void test_mode1(void)
{
    setup_mode1();
    color(0, BLUE);
    color(16 + 1, RED);
    color(16 + 2, GREEN);
    /* row 0: blank; row 1: tile 2 palette 1, hflipped tile 2, blank */
    vaddr(0x1000 + 32);
    vword(2 | 1 << 10);
    vword(2 | 1 << 10 | 0x4000);
    w(0x2C, 1);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(vdp_screen_w, 256);
    CHECK_EQ(vdp_screen_h, 224);
    CHECK_EQ(vdp_native_w, 256);
    /* line 1 shows BG row 1 of tile row 0, line 8 (row 7) tile row 1 */
    CHECK_EQ(fb(0, 6), 0x0000FF);
    CHECK_EQ(fb(0, 7), 0xFF0000);
    CHECK_EQ(fb(3, 7), 0xFF0000);
    CHECK_EQ(fb(4, 7), 0x00FF00);
    CHECK_EQ(fb(8, 7), 0x00FF00);
    CHECK_EQ(fb(12, 7), 0xFF0000);
    CHECK_EQ(fb(16, 7), 0x0000FF);

    /* horizontal scroll by 4 through the two-write latch */
    w(0x00, 0x80);
    w(0x0D, 4); w(0x0D, 0);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 7), 0x00FF00);
    w(0x00, 0x80);
    w(0x0D, 0); w(0x0D, 0);
    w(0x00, 0x0F);

    /* brightness and forced blank */
    w(0x00, 0x07);
    frame();
    CHECK_EQ(fb(0, 7), 0x7F0000);
    w(0x00, 0x00);
    frame();
    CHECK_EQ(fb(0, 7), 0);
    w(0x00, 0x8F);
    frame();
    CHECK_EQ(fb(0, 7), 0);
    w(0x00, 0x0F);

    /* BG2 beneath BG1, tile priority lifts it above */
    color(1, GREEN);
    vaddr(0x1400);
    for (int i = 0; i < 1024; i++) vword(1);
    w(0x2C, 3);
    frame();
    CHECK_EQ(fb(0, 7), 0xFF0000);
    vaddr(0x1400);
    w(0x00, 0x80);
    vaddr(0x1400);
    for (int i = 0; i < 1024; i++) vword(1 | 0x2000);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 7), 0x00FF00);

    /* VRAM writes during active display are dropped */
    snes_ppu_frame_start();
    vaddr(0x1400);
    vword(0);
    wp(0x16, 0x1400);
    snes_ppu_vblank();
    w(0x15, 0);
    wp(0x16, 0x1400);
    CHECK_EQ(snes_ppu_read(0x39) | snes_ppu_read(0x3A) << 8, 0x2001 | 0);
}

static void test_ports(void)
{
    snes_ppu_reset();
    w(0x00, 0x80);
    /* VRAM read prefetch: the first read returns the latched word */
    w(0x15, 0x80);
    wp(0x16, 0x100);
    vword(0x1234);
    vword(0x5678);
    wp(0x16, 0x100);
    CHECK_EQ(snes_ppu_read(0x39), 0x34);
    CHECK_EQ(snes_ppu_read(0x3A), 0x12);
    /* the fetch precedes the increment, so the first word comes back twice */
    CHECK_EQ(snes_ppu_read(0x39), 0x34);
    CHECK_EQ(snes_ppu_read(0x3A), 0x12);
    CHECK_EQ(snes_ppu_read(0x39), 0x78);
    CHECK_EQ(snes_ppu_read(0x3A), 0x56);
    /* increment on low, step 32 */
    w(0x15, 0x01);
    wp(0x16, 0x200);
    vword(0xAABB);
    vword(0xCCDD);
    wp(0x16, 0x220);
    CHECK_EQ(snes_ppu_read(0x39), 0xDD);
    /* remap mode 1: rrrrrrrrYYYccccc -> rrrrrrrrcccccYYY */
    w(0x15, 0x84);
    wp(0x16, 0x0123);                 /* row 1 (YYY), column 3 */
    vword(0xBEEF);
    w(0x15, 0x80);
    wp(0x16, 0x0119);
    CHECK_EQ(snes_ppu_read(0x39), 0xEF);

    /* OAM low table commits on the odd write */
    wp(0x02, 5);
    w(0x04, 0x11);
    wp(0x02, 5);
    CHECK_EQ(snes_ppu_read(0x38), 0);
    wp(0x02, 5);
    w(0x04, 0x11);
    w(0x04, 0x22);
    wp(0x02, 5);
    CHECK_EQ(snes_ppu_read(0x38), 0x11);
    CHECK_EQ(snes_ppu_read(0x38), 0x22);
    /* high table commits immediately */
    wp(0x02, 0x100);
    w(0x04, 0x77);
    wp(0x02, 0x100);
    CHECK_EQ(snes_ppu_read(0x38), 0x77);

    /* CGRAM round trip */
    w(0x21, 7);
    w(0x22, 0xCD);
    w(0x22, 0xFB);
    w(0x21, 7);
    CHECK_EQ(snes_ppu_read(0x3B), 0xCD);
    CHECK_EQ(snes_ppu_read(0x3B) & 0x7F, 0x7B);

    /* Multiply: M7A * high byte of M7B */
    w16(0x1B, 0x1234);
    w(0x1C, 0x02);
    CHECK_EQ(snes_ppu_read(0x34), 0x68);
    CHECK_EQ(snes_ppu_read(0x35), 0x24);
    CHECK_EQ(snes_ppu_read(0x36), 0x00);
    w16(0x1B, -2);
    w(0x1C, 0xFF);                     /* -1 */
    CHECK_EQ(snes_ppu_read(0x34), 2);
    w16(0x1B, 0x7FFF);
    w(0x1C, 0x80);                     /* -128 */
    CHECK_EQ(snes_ppu_read(0x36), 0xC0);

    /* H/V latch */
    snes_ppu_latch_hv(0x123, 0x0AB);
    CHECK(snes_ppu_read(0x3F) & 0x40);
    CHECK(!(snes_ppu_read(0x3F) & 0x40));
    snes_ppu_latch_hv(0x123, 0x0AB);
    (void)snes_ppu_read(0x3F);
    CHECK_EQ(snes_ppu_read(0x3C), 0x23);
    CHECK_EQ(snes_ppu_read(0x3C) & 1, 1);
    CHECK_EQ(snes_ppu_read(0x3D), 0xAB);
    CHECK_EQ(snes_ppu_read(0x3D) & 1, 0);
    CHECK_EQ(snes_ppu_read(0x3E) & 0xCF, 1);
}

static void test_mode7(void)
{
    snes_ppu_reset();
    w(0x00, 0x80);
    w(0x05, 7);
    /* map: (0,0)=1 (1,0)=2; chr 1 = colour 5, chr 2 = colour 6 */
    w(0x15, 0);
    wp(0x16, 0);
    for (int i = 0; i < 128 * 128; i++) w(0x18, 0);
    wp(0x16, 0);
    w(0x18, 1); w(0x18, 2);
    w(0x15, 0x80);
    for (int t = 1; t <= 2; t++) {
        wp(0x16, t << 6);
        for (int i = 0; i < 64; i++) w(0x19, 4 + t);
    }
    color(5, RED);
    color(6, GREEN);
    color(0, BLUE);
    w16(0x1B, 0x0100);
    w16(0x1C, 0);
    w16(0x1D, 0);
    w16(0x1E, 0x0100);
    w16(0x1F, 0);
    w16(0x20, 0);
    w(0x2C, 1);
    hide_sprites();
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(7, 0), 0xFF0000);
    CHECK_EQ(fb(8, 0), 0x00FF00);
    CHECK_EQ(fb(15, 6), 0x00FF00);
    CHECK_EQ(fb(16, 0), 0x0000FF);     /* tile 0, colour 0 */
    CHECK_EQ(fb(0, 7), 0x0000FF);      /* line 8 is y=8: map row 1 */

    /* per-line change, as HDMA would do it */
    snes_ppu_frame_start();
    snes_ppu_render_line(1);
    w16(0x0D, 8);                      /* M7HOFS = 8 */
    snes_ppu_render_line(2);
    w16(0x0D, 0);
    w16(0x1B, 0x0080);                 /* zoom in 2x */
    snes_ppu_render_line(3);
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(0, 1), 0x00FF00);
    CHECK_EQ(fb(15, 2), 0xFF0000);
    CHECK_EQ(fb(16, 2), 0x00FF00);
    snes_ppu_vblank();
    w16(0x1B, 0x0100);

    /* flip, wrap modes */
    w(0x1A, 0x01);
    frame();
    CHECK_EQ(fb(255, 0), 0xFF0000);
    CHECK_EQ(fb(247, 0), 0x00FF00);
    w(0x1A, 0x00);
    w16(0x0D, 1024);                   /* off the 1024 map, wraps to same */
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);
    w16(0x0D, -8);                     /* screen-over transparent */
    w(0x1A, 0x80);
    frame();
    CHECK_EQ(fb(0, 0), 0x0000FF);
    CHECK_EQ(fb(8, 0), 0xFF0000);
    w(0x1A, 0xC0);                     /* outside = tile 0, which is colour 0 */
    frame();
    CHECK_EQ(fb(0, 0), 0x0000FF);
    w16(0x0D, 0);
    w(0x1A, 0);

    /* EXTBG: pixels with bit 7 set are BG2 high priority, above OBJ-less BG1 */
    w(0x33, 0x40);
    w(0x2C, 2);
    color(5, GREEN);
    frame();
    CHECK_EQ(fb(0, 0), 0x00FF00);
    w(0x33, 0);
}

static void test_sprites(void)
{
    setup_mode1();
    color(0, BLUE);
    color(128 + 1, RED);
    color(128 + 2, GREEN);
    color(128 + 32 + 1, GREEN);
    w(0x01, 0x02);                     /* 8/16, base $4000 */
    tile4(0x4010, 0xFF, 0, 0, 0);
    tile4(0x4020, 0xF0, 0x0F, 0, 0);
    tile4(0x4030, 0xF0, 0, 0, 0);
    wp(0x02, 0);
    w(0x04, 10); w(0x04, 20); w(0x04, 1); w(0x04, 0x30);
    w(0x04, 100); w(0x04, 30); w(0x04, 2); w(0x04, 0x70);    /* hflip */
    w(0x04, 252); w(0x04, 40); w(0x04, 1); w(0x04, 0x30);    /* clipped at right */
    w(0x04, 252); w(0x04, 50); w(0x04, 1); w(0x04, 0x30);    /* negative X, below */
    /* sprite 4: 16x16 at (50,60), mirrored across tiles: chars 2,3 */
    w(0x04, 50); w(0x04, 60); w(0x04, 2); w(0x04, 0x70);
    /* sprite 5 overlaps sprite 0 from behind in OAM order, palette 2 */
    w(0x04, 14); w(0x04, 20); w(0x04, 1); w(0x04, 0x34);
    wp(0x02, 0x100);
    w(0x04, (1 << 6));                 /* sprite 3 X bit 8 */
    w(0x04, (1 << 0) << 1 | (0 << 2) );/* sprite 4 large */
    w(0x2C, 0x10);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(9, 20), 0x0000FF);
    CHECK_EQ(fb(10, 20), 0xFF0000);
    CHECK_EQ(fb(17, 20), 0xFF0000);    /* sprite 0 beats sprite 5 */
    CHECK_EQ(fb(18, 20), 0x00FF00);    /* sprite 5 alone */
    CHECK_EQ(fb(10, 19), 0x0000FF);
    CHECK_EQ(fb(10, 27), 0xFF0000);
    CHECK_EQ(fb(10, 28), 0x0000FF);
    CHECK_EQ(fb(100, 30), 0x00FF00);   /* flipped: colour 2 at left */
    CHECK_EQ(fb(104, 30), 0xFF0000);
    CHECK_EQ(fb(255, 40), 0xFF0000);
    CHECK_EQ(fb(251, 40), 0x0000FF);
    CHECK_EQ(fb(3, 50), 0xFF0000);     /* X=508 is -4 */
    CHECK_EQ(fb(4, 50), 0x0000FF);
    CHECK_EQ(fb(50, 60), 0x0000FF);    /* hflip large: char 3 on the right half */
    CHECK_EQ(fb(54, 60), 0xFF0000);
    CHECK_EQ(fb(58, 60), 0x00FF00);    /* char 2 flipped: colour 2 first */

    /* priority against BG */
    color(1, GREEN);
    w(0x00, 0x80);
    fill_map(0x1000, 1);
    w(0x00, 0x0F);
    w(0x2C, 0x11);
    wp(0x02, 0);
    w(0x04, 10); w(0x04, 20); w(0x04, 1); w(0x04, 0x00);     /* priority 0 */
    frame();
    CHECK_EQ(fb(11, 20), 0x00FF00);
    wp(0x02, 0);
    w(0x04, 10); w(0x04, 20); w(0x04, 1); w(0x04, 0x30);
    frame();
    CHECK_EQ(fb(11, 20), 0xFF0000);
    CHECK_EQ(snes_ppu_read(0x3E) & 0xC0, 0);

    /* 33 sprites on a line: range over; 32 of the 8-wide fit in 34 tiles */
    hide_sprites();
    wp(0x02, 0);
    for (int i = 0; i < 33; i++) { w(0x04, i * 7); w(0x04, 100); w(0x04, 1); w(0x04, 0x30); }
    frame();
    CHECK_EQ(snes_ppu_read(0x3E) & 0xC0, 0x40);

    /* 13 large (2 tiles wide... 16px) sprites: 26 tiles, no flags */
    hide_sprites();
    wp(0x02, 0);
    for (int i = 0; i < 18; i++) { w(0x04, i * 12); w(0x04, 100); w(0x04, 1); w(0x04, 0x30); }
    wp(0x02, 0x100);
    for (int i = 0; i < 5; i++) w(0x04, 0xAA);
    frame();
    CHECK_EQ(snes_ppu_read(0x3E) & 0xC0, 0x80);        /* 18*2 = 36 tiles */
    /* the budget is spent from the last sprite back, so sprite 0 lost tiles */
    CHECK_EQ(fb(208, 100), 0xFF0000);
    CHECK_EQ(fb(0, 100), 0x00FF00);

    /* flags still computed with rendering skipped */
    vdp_skip_render = 1;
    frame();
    vdp_skip_render = 0;
    CHECK_EQ(snes_ppu_read(0x3E) & 0xC0, 0x80);

    /* priority rotation: sprite 2 goes first */
    hide_sprites();
    wp(0x02, 0);
    w(0x04, 10); w(0x04, 20); w(0x04, 1); w(0x04, 0x30);
    w(0x04, 0); w(0x04, 240); w(0x04, 0); w(0x04, 0);
    w(0x04, 12); w(0x04, 20); w(0x04, 1); w(0x04, 0x32 + 0x00);
    color(128 + 16 + 1, GREEN);
    frame();
    CHECK_EQ(fb(12, 20), 0xFF0000);
    w(0x02, 4); w(0x03, 0x80);
    frame();
    CHECK_EQ(fb(12, 20), 0x00FF00);
}

static void test_color_math(void)
{
    setup_mode1();
    color(0, 0);
    color(1, RED);
    color(16 + 1, GREEN);
    fill_map(0x1000, 1);               /* BG1 palette 0: red */
    fill_map(0x1400, 1 | 1 << 10);     /* BG2 palette 1: green */

    /* add fixed colour */
    w(0x2C, 1);
    w(0x30, 0);
    w(0x31, 0x01);
    w(0x32, 0x9F);                     /* blue=31 */
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF00FF);

    /* add saturates */
    w(0x32, 0x3F);                     /* red=31 */
    frame();
    CHECK_EQ(fb(0, 0), 0xFF00FF);

    /* half */
    w(0x31, 0x41);
    w(0x32, 0x80);                     /* blue=0 */
    w(0x32, 0x20);                     /* red=0 */
    frame();
    CHECK_EQ(fb(0, 0), 0x7B0000);      /* 15 -> 0x7B */

    /* sub screen add */
    w(0x2D, 2);
    w(0x30, 2);
    w(0x31, 0x01);
    frame();
    CHECK_EQ(fb(0, 0), 0xFFFF00);

    /* subtract: red - sub (green) leaves red */
    w(0x31, 0x81);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);

    /* half does not apply against a transparent sub screen */
    w(0x2D, 0);
    w(0x31, 0x41);
    w(0x32, 0x4F);                     /* green=15 fixed backdrop */
    frame();
    CHECK_EQ(fb(0, 0), 0xFF7B00);

    /* colour window: math only inside 8..15, clip to black outside 0..100 */
    w(0x25, 0x20);                     /* colour window 1 enable */
    w(0x26, 8);
    w(0x27, 15);
    w(0x30, 0x12);
    frame();
    CHECK_EQ(fb(7, 0), 0xFF0000);
    CHECK_EQ(fb(8, 0), 0xFF7B00);
    CHECK_EQ(fb(16, 0), 0xFF0000);
    w(0x30, 0x42);                     /* black outside window, math on all */
    frame();
    CHECK_EQ(fb(7, 0), 0x007B00);      /* blacked, then math adds the sub screen */
    CHECK_EQ(fb(8, 0), 0xFF7B00);
    w(0x30, 0xC2);                     /* always black, math applies to black */
    w(0x31, 0x01);
    frame();
    CHECK_EQ(fb(8, 0), 0x007B00);
    w(0x25, 0);
    w(0x30, 0);
    w(0x31, 0);

    /* layer window with OR of window 1 and an inverted window 2 */
    w(0x2C, 3);
    w(0x2E, 1);                        /* BG1 masked on main */
    w(0x23, 0x0E);                     /* W1 en, W2 en + invert */
    w(0x26, 10); w(0x27, 20);
    w(0x28, 0);  w(0x29, 200);
    w(0x2A, 0);
    frame();
    CHECK_EQ(fb(9, 0), 0xFF0000);
    CHECK_EQ(fb(10, 0), 0x00FF00);     /* BG1 gone, BG2 shows */
    CHECK_EQ(fb(201, 0), 0x00FF00);
    w(0x2A, 1);                        /* AND: W1 inside and outside W2: empty */
    frame();
    CHECK_EQ(fb(10, 0), 0xFF0000);
    w(0x2E, 0);
    w(0x23, 0);

    /* OBJ palettes 0-3 ignore math, 4-7 obey */
    w(0x2C, 0x10);
    w(0x01, 0x02);
    tile4(0x4010, 0xFF, 0, 0, 0);
    color(128 + 1, RED);
    color(128 + 64 + 1, RED);
    wp(0x02, 0);
    w(0x04, 10); w(0x04, 20); w(0x04, 1); w(0x04, 0x30);
    w(0x04, 30); w(0x04, 20); w(0x04, 1); w(0x04, 0x38);
    w(0x31, 0x10);
    w(0x32, 0x9F);
    frame();
    CHECK_EQ(fb(10, 20), 0xFF7B00 & 0xFF0000);
    CHECK_EQ(fb(30, 20), 0xFF7BFF);
}

static void test_other_modes(void)
{
    /* Mode 0 palettes: BG2 uses colours 32+ */
    setup_mode1();
    w(0x00, 0x80);
    w(0x05, 0);
    color(32 + 1, RED);
    color(0, BLUE);
    tile4(0x3008, 0xFF, 0, 0, 0);      /* 2bpp tile 1 */
    fill_map(0x1400, 1);
    w(0x2C, 2);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);

    /* 16x16 tiles: map entry 1 gives tiles 1,2 / 17,18 */
    w(0x00, 0x80);
    w(0x05, 0x21);
    color(16 + 1, RED);
    color(16 + 2, GREEN);
    fill_map(0x1400, 1 | 1 << 10);
    tile4(0x3010, 0xFF, 0, 0, 0);      /* the 2bpp upload above clobbered it */
    tile4(0x3020, 0, 0xFF, 0, 0);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(8, 0), 0x00FF00);

    /* Mosaic: horizontal hold */
    w(0x00, 0x80);
    w(0x05, 1);
    fill_map(0x1000, 2 | 1 << 10);
    w(0x06, 0xF1);                     /* size 16, BG1 */
    w(0x2C, 1);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(15, 0), 0xFF0000);
    w(0x06, 0);

    /* Pseudo hi-res gives a 512-wide frame, sub on even dots, main on odd */
    w(0x33, 0x08);
    color(0, 0);
    w(0x00, 0x80);
    fill_map(0x1000, 1 | 1 << 10);
    w(0x00, 0x0F);
    w(0x2C, 1);
    w(0x2D, 0);
    frame();
    CHECK_EQ(vdp_screen_w, 512);
    CHECK_EQ(fb(0, 0), 0);
    CHECK_EQ(fb(1, 0), 0xFF0000);
    w(0x33, 0);

    /* Switching to hi-res mid-frame widens earlier lines */
    snes_ppu_frame_start();
    snes_ppu_render_line(1);
    w(0x33, 0x08);
    snes_ppu_render_line(2);
    w(0x33, 0);
    snes_ppu_render_line(3);
    CHECK_EQ(vdp_screen_w, 512);
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(511, 0), 0xFF0000);
    CHECK_EQ(fb(0, 1), 0);
    CHECK_EQ(fb(1, 1), 0xFF0000);
    CHECK_EQ(fb(0, 2), 0xFF0000);
    snes_ppu_vblank();

    /* Mode 5: 16-wide tiles at 512 */
    w(0x00, 0x80);
    w(0x05, 5);
    color(1, RED);
    color(2, GREEN);
    fill_map(0x1000, 1);
    w(0x2C, 1);
    w(0x2D, 1);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(vdp_screen_w, 512);
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(1, 0), 0xFF0000);
    CHECK_EQ(fb(7, 0), 0xFF0000);
    CHECK_EQ(fb(8, 0), 0xFF0000);      /* tile 2, colour 1 half */
    CHECK_EQ(fb(12, 0), 0x00FF00);     /* tile 2, colour 2 half */

    /* Mode 3 direct colour: index $C0 is blue 3 of 3 from BBGGGRRR */
    w(0x00, 0x80);
    w(0x05, 3);
    w(0x30, 1);
    w(0x0B, 0x30);                     /* BG1 chr 0 */
    vaddr(0x20);                       /* tile 1 of 8bpp */
    for (int plane = 0; plane < 4; plane++)
        for (int i = 0; i < 8; i++) vword(plane == 3 ? 0xFFFF : 0);
    fill_map(0x1000, 1);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0x0000C6);
    w(0x30, 0);

    /* Mode 2 offset-per-tile: column 1 of BG1 scrolled by BG3 data */
    w(0x00, 0x80);
    w(0x05, 2);
    w(0x09, 0x18);                     /* BG3 map $1800 */
    w(0x0B, 0x32);
    color(16 + 1, RED);
    color(1, GREEN);
    fill_map(0x1800, 0);
    fill_map(0x1000, 0);
    vaddr(0x1000 + 0);
    for (int i = 0; i < 16; i++) vword(i == 0 ? (1 | 1 << 10) : 0);
    vaddr(0x1000 + 2);
    vword(1);
    /* BG3 column 0 H-scroll for BG1 screen column 1: makes it read map col 0 */
    vaddr(0x1800);
    vword(0x2000 | 0x3F8);
    w(0x2C, 1);
    w(0x00, 0x0F);
    frame();
    CHECK_EQ(fb(0, 0), 0xFF0000);
    CHECK_EQ(fb(8, 0), 0xFF0000);
    CHECK_EQ(fb(16, 0), 0x00FF00);
}

static void bench(void)
{
    setup_mode1();
    color(1, RED);
    color(2, GREEN);
    color(16 + 1, BLUE);
    fill_map(0x1000, 2 | 1 << 10);
    fill_map(0x1400, 1);
    w(0x01, 0x02);
    tile4(0x4010, 0xFF, 0, 0, 0);
    wp(0x02, 0);
    for (int i = 0; i < 128; i++) { w(0x04, i * 2); w(0x04, i); w(0x04, 1); w(0x04, 0x30); }
    w(0x2C, 0x13);
    w(0x2D, 0x02);
    w(0x30, 2);
    w(0x31, 0x21);
    w(0x00, 0x0F);
    clock_t t0 = clock();
    int n = 3000;
    for (int i = 0; i < n; i++) frame();
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("Mode 1 + sprites + math: %.0f frames/s\n", n / s);

    w(0x00, 0x80);
    w(0x05, 7);
    w16(0x1B, 0x0100);
    w16(0x1E, 0x0100);
    w(0x00, 0x0F);
    w(0x2C, 0x11);
    t0 = clock();
    for (int i = 0; i < n; i++) frame();
    s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("Mode 7 + sprites: %.0f frames/s\n", n / s);
}

int main(void)
{
    test_ports();
    test_mode1();
    test_mode7();
    test_sprites();
    test_color_math();
    test_other_modes();
    bench();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}

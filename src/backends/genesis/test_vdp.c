/* Standalone VDP test: clang -O2 -Wall -Wextra -std=c11 src/vdp.c src/test_vdp.c */
#include "genesis.h"
#include <stdio.h>

static u16 fake_rom[0x8000];
static int fake_cycle;
u16 bus_read16_dma(u32 a) { return fake_rom[(a >> 1) & 0x7FFF]; }
int sys_line_cycle(void) { return fake_cycle; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void reg(int r, int v) { vdp_write_ctrl((u16)(0x8000 | (r << 8) | (v & 0xFF))); }
static void set_addr(u32 a, int cd)
{
    vdp_write_ctrl((u16)(((cd & 3) << 14) | (a & 0x3FFF)));
    vdp_write_ctrl((u16)(((cd & 0x3C) << 2) | ((a >> 14) & 3)));
}
static void frame(void) { for (int l = 0; l < LINES_PER_FRAME; l++) vdp_begin_line(l); }
static u32 px(int x, int y) { return vdp_framebuffer[y * VDP_FB_STRIDE + x]; }

int main(void)
{
    vdp_widescreen = 0;
    vdp_no_sprite_limit = 0;
    vdp_init();
    reg(0, 0x14); reg(1, 0x74); reg(2, 0xC000 >> 10); reg(3, 0xB000 >> 10);
    reg(4, 0xE000 >> 13); reg(5, 0xF800 >> 9); reg(7, 0x00); reg(10, 0xFF);
    reg(11, 0x00); reg(12, 0x81); reg(13, 0xFC00 >> 10); reg(15, 2);
    reg(16, 0x01); reg(17, 0); reg(18, 0);

    /* palette: 0 black, 1 red, 2 green, 0x11 blue, backdrop idx 0x02 later */
    set_addr(0, 0x03);
    vdp_write_data(0x0000); vdp_write_data(0x000E); vdp_write_data(0x00E0);
    set_addr(0x22, 0x03); vdp_write_data(0x0E00);
    set_addr(0x04, 0x08);
    CHECK(vdp_read_data() == 0x00E0);

    /* tile 1: solid color 1; tile 2: solid color 2 (via 68k DMA) */
    set_addr(0x20, 0x01);
    for (int i = 0; i < 16; i++) vdp_write_data(0x1111);
    for (int i = 0; i < 16; i++) fake_rom[0x100 + i] = 0x2222;
    reg(19, 16); reg(20, 0); reg(21, 0x100 & 0xFF); reg(22, 0x100 >> 8); reg(23, 0);
    set_addr(0x40, 0x21);
    set_addr(0x40, 0x00);
    CHECK(vdp_read_data() == 0x2222);

    /* VRAM fill: plane B nametable (0xE000) with 0 (already 0), plane A entry 0 -> tile 1 */
    set_addr(0xC000, 0x01); vdp_write_data(0x0001); vdp_write_data(0x0001); /* A (0,0),(1,0) = tile 1 low prio */
    set_addr(0xE000, 0x01); vdp_write_data(0x8002);   /* B (0,0) = tile 2 high prio */
    set_addr(0xE002, 0x01); vdp_write_data(0x0002);   /* B (1,0) = tile 2 low prio */

    /* fill test at 0x8000 */
    reg(15, 1); reg(19, 0x20); reg(20, 0); reg(23, 0x80);
    set_addr(0x8000, 0x21); vdp_write_data(0xAB00);
    set_addr(0x8010, 0x00); CHECK(vdp_read_data() == 0xABAB);
    /* copy 0x8000 -> 0x9000 */
    reg(19, 0x10); reg(20, 0); reg(21, 0x00); reg(22, 0x80); reg(23, 0xC0);
    set_addr(0x9000, 0x30);
    set_addr(0x9008, 0x00); CHECK(vdp_read_data() == 0xABAB);

    reg(15, 2);
    /* odd address write byte-swap */
    set_addr(0xA001, 0x01); vdp_write_data(0x1234);
    set_addr(0xA000, 0x00); CHECK(vdp_read_data() == 0x3412);

    /* sprite 0: tile 1 palette 1 (blue at color 1), 1x1 at screen (100,50), high prio */
    set_addr(0xF800, 0x01);
    vdp_write_data(128 + 50); vdp_write_data(0x0000); vdp_write_data(0xA001); vdp_write_data(128 + 100);

    frame();
    CHECK(vdp_screen_w == 320 && vdp_screen_h == 224);
    CHECK(px(0, 0) == 0x00FF00);   /* B high (green) beats A low (red) */
    CHECK(px(8, 0) == 0xFF0000);   /* A low over B low */
    CHECK(px(16, 0) == 0x000000);  /* backdrop */
    CHECK(px(100, 50) == 0x0000FF); /* sprite */
    CHECK(px(107, 57) == 0x0000FF);
    CHECK(px(108, 50) == 0x000000);

    /* horizontal full scroll of plane A by 4 */
    set_addr(0xFC00, 0x01); vdp_write_data(4);
    frame();
    CHECK(px(12, 0) == 0xFF0000 && px(4, 0) == 0x00FF00);

    /* window covering left 16 px on top of A: window entry 0 -> empty tile 0 */
    reg(17, 0x01);
    frame();
    CHECK(px(12, 0) == 0x00FF00 && px(20, 0) == 0x000000);
    reg(17, 0);

    /* interrupts */
    vdp_irq_ack(6); vdp_irq_ack(4);
    vdp_begin_line(0);
    CHECK(vdp_irq_pending() == 0);
    vdp_begin_line(224);
    CHECK(vdp_irq_pending() == 6);
    CHECK(vdp_zint_pending() == 1);
    CHECK(vdp_read_ctrl() & 0x88);
    vdp_irq_ack(6);
    CHECK(vdp_irq_pending() == 0);
    /* H-int every 4 lines */
    reg(10, 3);
    for (int l = 225; l < 262; l++) vdp_begin_line(l);
    int hints = 0;
    for (int l = 0; l < 224; l++) {
        vdp_begin_line(l);
        if (vdp_irq_pending() == 4) { hints++; vdp_irq_ack(4); }
    }
    CHECK(hints == 56);
    /* pending V-int fires when enabled later */
    reg(1, 0x54);
    vdp_begin_line(224);
    CHECK(vdp_irq_pending() == 0 || vdp_irq_pending() == 4);
    reg(1, 0x74);
    CHECK(vdp_irq_pending() == 6);

    /* HV counter */
    fake_cycle = 0;
    vdp_begin_line(0x10);
    CHECK((vdp_read_hv() >> 8) == 0x10);
    vdp_begin_line(0xF0);
    CHECK((vdp_read_hv() >> 8) == 0xEA);

    /* shadow/highlight: backdrop shadowed when planes low priority */
    reg(12, 0x89);
    frame();
    reg(7, 2); frame();
    CHECK(px(30, 0) == 0x007F00);   /* backdrop shadowed */
    CHECK(px(4, 0) == 0x00FF00);   /* B high -> normal */
    CHECK(px(12, 0) == 0x7F0000);  /* A low, no prio -> shadowed red */

    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("all VDP tests passed\n");
    return 0;
}

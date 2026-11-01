/*
 * vdp.c - Sega Mega Drive / Genesis VDP (315-5313) emulation, Mode 5 only.
 *
 * Line-based renderer: planes A/B (+window), sprites, priority, shadow /
 * highlight, line/cell/full scroll, 2-cell column vscroll. DMA is executed
 * instantly. Interrupt bookkeeping per scanline via vdp_begin_line().
 */
#include "genesis.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>

u32 vdp_framebuffer[VDP_FB_STRIDE * 240];
int vdp_screen_w = 320, vdp_screen_h = 224;

EMU_STATE static u8  vram[0x10000];
EMU_STATE static u16 cram[64];
EMU_STATE static u16 vsram[40];
EMU_STATE static u8  regs[32];

int vdp_widescreen = 1;
#ifdef GENEXIA_OS9
int vdp_altivec;
unsigned vdp_altivec_lines;
#endif
EMU_STATE int vdp_native_w = 320;
EMU_STATE int vdp_view_x = 0;
EMU_STATE static int out_w = 320, pad_l = 0, pad_r = 0;
int vdp_layout_debug;
int vdp_no_sprite_limit = 1;
int vdp_skip_render;
int vdp_hud_rules = 1;
u8 vdp_fx_mask[240 * VDP_FB_STRIDE];
long vdp_stat_limited_lines, vdp_stat_dot_lines;
EMU_STATE static u8 line_sparse[240];     /* native view mostly backdrop: margins get backdrop too */
EMU_STATE static int hud_hs_line;         /* first stage line below the HUD window */

EMU_STATE static u32 pal_n[64], pal_s[64], pal_h[64];   /* normal / shadow / highlight */

EMU_STATE static int  ctrl_pending;
EMU_STATE static u8   code;
EMU_STATE static u16  addr;
EMU_STATE static int  fill_pending;

EMU_STATE static int  cur_line;
EMU_STATE static int  active_h = 224;
EMU_STATE static int  vblank_flag;
EMU_STATE static int  vint_flag;          /* status bit 7 */
EMU_STATE static int  spr_overflow;       /* status bit 6 */
EMU_STATE static int  spr_collision;      /* status bit 5 */
EMU_STATE static int  vint_pending, hint_pending;
static inline void irq_update(void);
EMU_STATE static int  hcounter;
EMU_STATE static int  prev_dot_overflow;

/* ------------------------------------------------------------ helpers */

static const u8 ramp[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };

static void cram_update(int i)
{
    u16 c = cram[i];
    int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    int R = ramp[r], G = ramp[g], B = ramp[b];
    pal_n[i] = ((u32)R << 16) | ((u32)G << 8) | (u32)B;
    pal_s[i] = ((u32)(R >> 1) << 16) | ((u32)(G >> 1) << 8) | (u32)(B >> 1);
    pal_h[i] = ((u32)(128 + (R >> 1)) << 16) | ((u32)(128 + (G >> 1)) << 8) | (u32)(128 + (B >> 1));
}

static inline int h40(void) { return (regs[12] & 0x81) != 0; }
static inline int screen_width(void) { return h40() ? 320 : 256; }

/* write one word to the current target, then auto-increment */
static void port_write(u16 v)
{
    switch (code & 0x0F) {
    case 1: { /* VRAM */
        u16 a = addr;
        if (a & 1) v = (u16)((v >> 8) | (v << 8));
        vram[a & 0xFFFE] = (u8)(v >> 8);
        vram[a | 1]      = (u8)v;
        break; }
    case 3: { /* CRAM */
        int i = (addr >> 1) & 0x3F;
        cram[i] = v & 0x0EEE;
        cram_update(i);
        break; }
    case 5: { /* VSRAM */
        int i = (addr >> 1) & 0x3F;
        if (i < 40) vsram[i] = v & 0x07FF;
        break; }
    default: break;
    }
    addr = (u16)(addr + regs[15]);
}

static u32 dma_length(void)
{
    u32 len = regs[19] | ((u32)regs[20] << 8);
    return len ? len : 0x10000;
}

static void dma_finish(u32 len)
{
    u32 src = regs[21] | ((u32)regs[22] << 8);
    src = (src + len) & 0xFFFF;
    regs[21] = (u8)src; regs[22] = (u8)(src >> 8);
    regs[19] = regs[20] = 0;
}

static void dma_68k(void)
{
    u32 len = dma_length();
    u32 hi  = (u32)(regs[23] & 0x7F) << 17;
    u32 src = regs[21] | ((u32)regs[22] << 8);
    for (u32 i = 0; i < len; i++) {
        u16 w = bus_read16_dma(hi | ((src & 0xFFFF) << 1));
        port_write(w);
        src++;
    }
    dma_finish(len);
}

static void dma_copy(void)
{
    u32 len = dma_length();
    u16 src = (u16)(regs[21] | (regs[22] << 8));
    for (u32 i = 0; i < len; i++) {
        vram[addr] = vram[src];
        src++;
        addr = (u16)(addr + regs[15]);
    }
    dma_finish(len);
}

static void dma_fill(u16 data)
{
    u32 len = dma_length();
    if ((code & 0x0F) == 1) {
        u8 b = (u8)(data >> 8);
        for (u32 i = 0; i < len; i++) {
            vram[addr ^ 1] = b;
            addr = (u16)(addr + regs[15]);
        }
    } else {
        for (u32 i = 0; i < len; i++) port_write(data);
    }
    dma_finish(len);
}

/* ------------------------------------------------------------ ports */

void vdp_init(void)
{
    memset(vram, 0, sizeof vram);
    memset(cram, 0, sizeof cram);
    memset(vsram, 0, sizeof vsram);
    memset(regs, 0, sizeof regs);
    memset(vdp_framebuffer, 0, sizeof vdp_framebuffer);
    vdp_native_w = 320; out_w = 320; vdp_view_x = pad_l = pad_r = 0;
    for (int i = 0; i < 64; i++) cram_update(i);
    regs[15] = 2;
    ctrl_pending = 0; code = 0; addr = 0; fill_pending = 0;
    cur_line = 0; active_h = 224; vblank_flag = 0; vint_flag = 0;
    spr_overflow = spr_collision = 0;
    vint_pending = hint_pending = 0;
    vdp_irq_level = 0;
    hcounter = 0; prev_dot_overflow = 0;
    vdp_screen_w = 320; vdp_screen_h = 224;
}

void vdp_write_ctrl(u16 v)
{
    if (!ctrl_pending) {
        if ((v & 0xC000) == 0x8000) {
            int r = (v >> 8) & 0x1F;
            if (r < 24) regs[r] = (u8)v;
            if (r < 2) irq_update();
        } else {
            ctrl_pending = 1;
        }
        addr = (u16)((addr & 0xC000) | (v & 0x3FFF));
        code = (u8)((code & 0x3C) | ((v >> 14) & 3));
        return;
    }
    ctrl_pending = 0;
    addr = (u16)((addr & 0x3FFF) | ((v & 3) << 14));
    code = (u8)((code & 0x03) | ((v >> 2) & 0x3C));
    if ((code & 0x20) && (regs[1] & 0x10)) {
        switch (regs[23] >> 6) {
        case 0: case 1: dma_68k(); break;
        case 2: fill_pending = 1; break;
        case 3: dma_copy(); break;
        }
    }
}

void vdp_write_data(u16 v)
{
    ctrl_pending = 0;
    port_write(v);
    if (fill_pending) {
        fill_pending = 0;
        dma_fill(v);
    }
}

u16 vdp_read_data(void)
{
    u16 r = 0;
    ctrl_pending = 0;
    switch (code & 0x0F) {
    case 0x0: { u16 a = addr & 0xFFFE; r = (u16)((vram[a] << 8) | vram[a | 1]); break; }
    case 0x4: { int i = (addr >> 1) & 0x3F; r = vsram[i < 40 ? i : 0]; break; }
    case 0x8: r = cram[(addr >> 1) & 0x3F]; break;
    case 0xC: r = vram[addr ^ 1]; break;  /* undocumented 8-bit VRAM read */
    default: break;
    }
    addr = (u16)(addr + regs[15]);
    return r;
}

u16 vdp_read_ctrl(void)
{
    u16 s = 0x3400 | 0x0200;  /* fifo empty */
    if (vint_flag) s |= 0x80;
    if (spr_overflow) s |= 0x40;
    if (spr_collision) s |= 0x20;
    if (vblank_flag || !(regs[1] & 0x40)) s |= 0x08;
    if (sys_line_cycle() >= 400) s |= 0x04;
    ctrl_pending = 0;
    spr_overflow = 0;
    spr_collision = 0;
    return s;
}

u16 vdp_read_hv(void)
{
    int cyc = sys_line_cycle();
    if (cyc < 0) cyc = 0;
    if (cyc > 487) cyc = 487;
    int h;
    if (h40()) {
        int idx = cyc * 211 / 488;
        h = idx <= 0xB6 ? idx : idx - 0xB7 + 0xE4;
    } else {
        int idx = cyc * 171 / 488;
        h = idx <= 0x93 ? idx : idx - 0x94 + 0xE9;
    }
    int v;
    if (active_h == 224) v = (cur_line > 0xEA ? cur_line - 6 : cur_line) & 0xFF;
    else v = cur_line & 0xFF;
    return (u16)((v << 8) | (h & 0xFF));
}

/* ------------------------------------------------------------ rendering */

/* Output geometry for the current frame. Game x=0 lands at output x=view_x;
 * output pixels [pad_l, out_w-pad_r) are drawn, the rest is black. */

#define MAXW 512
#define MARGIN 16
static u8 line_a[MARGIN + MAXW + 32] __attribute__((aligned(16)));
static u8 line_b[MARGIN + MAXW + 32] __attribute__((aligned(16)));
static u8 line_s[MAXW] __attribute__((aligned(16)));

static inline u16 vram_w(u32 a)
{
    a &= 0xFFFE;
    return (u16)((vram[a] << 8) | vram[a | 1]);
}

static inline void draw_tile_row(u8 *dst, u16 e, int fine)
{
    int r = (e & 0x1000) ? 7 - fine : fine;
    const u8 *p = &vram[((e & 0x7FF) << 5) + (r << 2)];
    u8 attr = (u8)(((e >> 8) & 0x80) | ((e >> 9) & 0x30));
    if (e & 0x0800) {
        for (int i = 0; i < 4; i++) {
            dst[7 - 2 * i] = attr | (p[i] >> 4);
            dst[6 - 2 * i] = attr | (p[i] & 15);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            dst[2 * i]     = attr | (p[i] >> 4);
            dst[2 * i + 1] = attr | (p[i] & 15);
        }
    }
}

static inline int size_cells(int c) { return c == 1 ? 64 : c == 3 ? 128 : 32; }

static inline u32 plane_base(int is_b)
{
    return is_b ? ((u32)(regs[4] & 7) << 13) : ((u32)(regs[2] & 0x38) << 10);
}

static int plane_hscroll(int is_b, int line)
{
    u32 hsbase = (u32)(regs[13] & 0x3F) << 10;
    int hl;
    switch (regs[11] & 3) {
    case 0:  hl = 0; break;
    case 1:  hl = line & 7; break;
    case 2:  hl = line & ~7; break;
    default: hl = line; break;
    }
    return vram_w(hsbase + (u32)hl * 4 + (is_b ? 2 : 0)) & 0x3FF;
}

/* render output pixels [0, out_w) of a plane line; buf index = output x */
static void render_plane_ex(u8 *buf, int is_b, int line, int hs, int clamp_top);
static void render_plane(u8 *buf, int is_b, int line) { render_plane_ex(buf, is_b, line, plane_hscroll(is_b, line), 0); }
/* UMK3 fights: extent of the art on each plane row. Layers narrower than the
 * wide view (the Desert's 320-px sky) repeat in the margins instead of ending */
static int plane_fill;
static s16 ext_a[2][128], ext_b[2][128];
EMU_STATE static int rest_vs[2] = { 0x7FFF, 0x7FFF };   /* resting vertical scroll (signed), widescreen fill */

/* hs_line: the line whose horizontal scroll value is used */
/* hs: horizontal scroll value to use. clamp_top: never show plane rows above
 * the stage's resting top (camera raised during jumps) - repeat the top row */
static void render_plane_ex(u8 *buf, int is_b, int line, int hs, int clamp_top)
{
    int wc = size_cells(regs[16] & 3);
    int hc = size_cells((regs[16] >> 4) & 3);
    u32 base = plane_base(is_b);
    hs &= 0x3FF;
    int shift = hs & 15;
    int colmode = regs[11] & 4;
    int ncols_native = vdp_native_w / 16;
    int wmask = wc * 8 - 1, hmask = hc * 8 - 1;

    /* columns in game coordinates covering output [0, out_w) */
    int c0 = (-vdp_view_x - shift) >> 4;   /* arithmetic shift = floor */
    int c1 = (out_w - vdp_view_x - shift + 15) >> 4;
    for (int c = c0 - 1; c <= c1; c++) {
        int vs;
        if (colmode) {
            if (c < 0) vs = h40() ? (vsram[38] & vsram[39]) : 0;
            else vs = vsram[(c < ncols_native ? c : ncols_native - 1) * 2 + is_b];
        } else vs = vsram[is_b];
        int y = (line + (vs & 0x3FF)) & hmask;
        if (clamp_top && rest_vs[is_b] != 0x7FFF) {
            int sv = (s16)(vs << 5) >> 5;                  /* 11-bit signed */
            if (line + sv < rest_vs[is_b]) y = rest_vs[is_b] & hmask;
        }
        int row = y >> 3, fine = y & 7;
        int sx = c * 16 + shift;                /* game x of this column */
        int px = (sx - hs) & wmask;
        for (int t = 0; t < 2; t++) {
            int ox = sx + t * 8 + vdp_view_x;   /* output x */
            if (ox < -8 || ox >= out_w + 8) continue;
            int tc = ((px >> 3) + t) & (wc - 1);
            if (plane_fill && (ox < vdp_view_x || ox >= vdp_view_x + vdp_native_w)) {
                int a = ext_a[is_b][row], b = ext_b[is_b][row], W = b - a + 1;
                if (a >= 0 && W >= 8 && W < wc && (tc < a || tc > b)) tc = a + (((tc - a) % W) + W) % W;
            }
            u16 e = vram_w(base + (u32)(row * wc + tc) * 2);
            draw_tile_row(buf + MARGIN + ox, e, fine);
        }
    }
}

static void render_window(u8 *buf, int line, int width)
{
    int wv = regs[18] & 0x1F, down = regs[18] & 0x80;
    int wh = regs[17] & 0x1F, right = regs[17] & 0x80;
    int x0, x1;
    int vin = down ? (line >= wv * 8) : (line < wv * 8);
    if (vin) { x0 = 0; x1 = width; }
    else {
        int hp = wh * 16;
        if (hp > width) hp = width;
        if (right) { x0 = hp; x1 = width; }
        else { x0 = 0; x1 = hp; }
    }
    if (x0 >= x1) return;
    int is40 = h40();
    u32 base = is40 ? ((u32)(regs[3] & 0x3C) << 10) : ((u32)(regs[3] & 0x3E) << 10);
    int ww = is40 ? 64 : 32;
    int row = (line >> 3), fine = line & 7;
    for (int tx = x0 >> 3; tx < (x1 >> 3); tx++) {
        u16 e = vram_w(base + (u32)(row * ww + tx) * 2);
        draw_tile_row(buf + MARGIN + vdp_view_x + tx * 8, e, fine);
    }
}

/* Widescreen: sprites that belong to the screen, not the stage. Found by
 * logging every sprite against camera motion over 120k frames of play:
 * palette 0 with these tile ranges never move with the camera (fighter
 * names and round timer, end-of-round TIME/VITAL/BONUS text, round
 * announcements). Other palette-0 sprites are in-world effects. */
static inline int is_screen_sprite(u16 attr)
{
    if (out_w <= vdp_native_w) return 0;
    int t = attr & 0x7FF, pal = (attr >> 13) & 3;
    if (vdp_hud_rules == 1)
        return pal == 0 && (t < 0x100 || (t >= 0x480 && t < 0x4C0) || (t >= 0x680 && t < 0x700));
    if (vdp_hud_rules == 2)      /* UMK3: same method, logged over 160k frames */
        return (pal == 3 && t >= 0x400 && t < 0x500) || (pal == 2 && t >= 0x780 && t < 0x7A0);
    return 0;
}
static inline int hud_x_now(void) { return (out_w - vdp_native_w) / 2; }

static void render_sprites(int line)
{
    memset(line_s, 0, (size_t)out_w);
    int is40 = h40();
    u32 sat = is40 ? ((u32)(regs[5] & 0x7E) << 9) : ((u32)(regs[5] & 0x7F) << 9);
    int max_total = is40 ? 80 : 64;
    int max_line  = is40 ? 20 : 16;
    int dots_left = vdp_native_w;
    if (vdp_no_sprite_limit) {           /* enhancement: no per-line limits, no flicker */
        max_line = max_total;
        dots_left = 1 << 20;
    } else if (out_w > vdp_native_w) {   /* widescreen: room for the extra sprites */
        max_line += max_line / 2;
        dots_left = out_w + vdp_native_w / 2;
    }
    int count = 0, hw_dots = 0, dots_flagged = 0;
    int mask_armed = prev_dot_overflow;
    int masked = 0;
    int dot_ovf = 0;
    int idx = 0;

    for (int n = 0; n < max_total; n++) {
        const u8 *s = &vram[(sat + (u32)idx * 8) & 0xFFFF];
        int y = ((s[0] << 8) | s[1]) & 0x1FF;
        int hsz = ((s[2] >> 2) & 3) + 1;
        int vsz = (s[2] & 3) + 1;
        int link = s[3] & 0x7F;
        int dy = line + 128 - y;
        if (dy >= 0 && dy < vsz * 8) {
            if (++count > max_line) { spr_overflow = 1; break; }
            if (count == (is40 ? 21 : 17)) vdp_stat_limited_lines++;   /* hardware would drop here */
            u16 attr = (u16)((s[4] << 8) | s[5]);
            int xraw = ((s[6] << 8) | s[7]) & 0x1FF;
            if (xraw) mask_armed = 1;
            else if (mask_armed) masked = 1;

            int w = hsz * 8;
            int draw_w = w;
            hw_dots += w;
            if (!dots_flagged && hw_dots > vdp_native_w) { vdp_stat_dot_lines++; dots_flagged = 1; }
            if (w >= dots_left) { draw_w = dots_left; dot_ovf = (w > dots_left); dots_left = 0; }
            else dots_left -= w;

            if (!masked && draw_w > 0) {
                if (attr & 0x1000) dy = vsz * 8 - 1 - dy;
                int cy = dy >> 3, fine = dy & 7;
                int hf = attr & 0x0800;
                u8 pa = (u8)(((attr >> 8) & 0x80) | ((attr >> 9) & 0x30));
                int sx = xraw - 128 + (is_screen_sprite(attr) ? hud_x_now() : vdp_view_x);
                for (int j = 0; j < hsz && j * 8 < draw_w; j++) {
                    int cx = hf ? hsz - 1 - j : j;
                    int tile = ((attr & 0x7FF) + cx * vsz + cy) & 0x7FF;
                    const u8 *p = &vram[(tile << 5) + (fine << 2)];
                    for (int k = 0; k < 8 && j * 8 + k < draw_w; k++) {
                        int pk = hf ? 7 - k : k;
                        int c = (pk & 1) ? (p[pk >> 1] & 15) : (p[pk >> 1] >> 4);
                        int x = sx + j * 8 + k;
                        if (!c || x < 0 || x >= out_w) continue;
                        if (line_s[x] & 15) { spr_collision = 1; continue; }
                        line_s[x] = pa | (u8)c;
                    }
                }
            }
            if (dots_left == 0) { if (dot_ovf) spr_overflow = 1; break; }
        }
        if (link == 0 || link >= max_total) break;
        idx = link;
    }
    prev_dot_overflow = dot_ovf;
}

/* ---------------------------------------------------- widescreen layout */

/* does this plane tile row contain any visible pixel? */
static inline int tile_row_used(u16 e, int fine)
{
    int r = (e & 0x1000) ? 7 - fine : fine;
    const u8 *p = &vram[((e & 0x7FF) << 5) + (r << 2)];
    return (p[0] | p[1] | p[2] | p[3]) != 0;
}

static int window_covers_line(int line)
{
    int wv = regs[18] & 0x1F, down = regs[18] & 0x80;
    return down ? (line >= wv * 8) : (line < wv * 8);
}

/* How far (pixels, up to `lim`) stage graphics continue to the left/right
 * of the game's view on this line, counting a column as present when either
 * plane has pixels there (a sparse foreground prop ending is fine: the
 * background shows through). Returns 0 if the line is mostly empty inside
 * the view, i.e. it shows backdrop anyway and must not constrain. */
static int line_extent(int line, int lim, int *left, int *right)
{
    int wc = size_cells(regs[16] & 3);
    int hc = size_cells((regs[16] >> 4) & 3);
    int wpx = wc * 8;
    u32 base[2]; int x0[2], row[2], fine[2];
    for (int p = 0; p < 2; p++) {
        base[p] = plane_base(p);
        x0[p] = (-plane_hscroll(p, line)) & (wpx - 1);   /* plane x at game x = 0 */
        int y = (line + (vsram[p] & 0x3FF)) & (hc * 8 - 1);
        row[p] = y >> 3; fine[p] = y & 7;
    }
    #define USEDP(p, gx) tile_row_used(vram_w(base[p] + (u32)(row[p] * wc + (((x0[p] + (gx)) & (wpx - 1)) >> 3)) * 2), fine[p])
    #define USED(gx) (USEDP(0, gx) || USEDP(1, gx))
    int tiles = vdp_native_w / 8, used = 0;
    for (int t = 0; t < tiles; t++) used += USED(t * 8);
    if (line < 240) line_sparse[line] = used * 4 < tiles;
    if (used * 4 < tiles * 3) return 0;
    /* never wrap onto the other side */
    int room = (wpx - vdp_native_w) / 2;
    if (lim > room) lim = room;
    /* pixel-exact: walk outwards one pixel at a time (each plane has its own
     * fine scroll, so tile edges differ per plane) until 16 empty pixels */
    int l = 0, r = 0, run = 0;
    for (int d = 1; d <= lim && run < 16; d++) {
        if (USED(-d)) { l = d; run = 0; } else run++;
    }
    if (run < 16) l = lim;
    run = 0;
    for (int d = 1; d <= lim && run < 16; d++) {
        if (USED(vdp_native_w - 1 + d)) { r = d; run = 0; } else run++;
    }
    if (run < 16) r = lim;
    #undef USED
    #undef USEDP
    *left = l;
    *right = r;
    return 1;
}

/* UMK3 fight screen? Its health bars are palette 3 sprites at tiles $400-$44F
 * near the top of the screen; menus, bios and the title have none of these. */
static int umk3_in_fight(void)
{
    u32 sat = h40() ? ((u32)(regs[5] & 0x7E) << 9) : ((u32)(regs[5] & 0x7F) << 9);
    int idx = 0, n = 0;
    for (int k = 0; k < 80; k++) {
        const u8 *sp = &vram[(sat + (u32)idx * 8) & 0xFFFF];
        int y = (((sp[0] << 8) | sp[1]) & 0x1FF) - 128, attr = (sp[4] << 8) | sp[5], t = attr & 0x7FF;
        if (((attr >> 13) & 3) == 3 && t >= 0x400 && t < 0x450 && y >= 0 && y < 48) n++;
        int link = sp[3] & 0x7F;
        if (!link) break;
        idx = link;
    }
    return n >= 4;
}

/* first and last column holding art, per plane row */
static void plane_extents(void)
{
    int wc = size_cells(regs[16] & 3), hc = size_cells((regs[16] >> 4) & 3);
    for (int p = 0; p < 2; p++) {
        u32 base = plane_base(p);
        for (int r = 0; r < hc; r++) {
            int a = -1, b = -1;
            for (int c = 0; c < wc; c++) {
                const u8 *t = &vram[(vram_w(base + (u32)(r * wc + c) * 2) & 0x7FF) << 5];
                int used = 0;
                for (int i = 0; i < 32 && !used; i++) used = t[i] != 0;
                if (used) { if (a < 0) a = c; b = c; }
            }
            ext_a[p][r] = (s16)a; ext_b[p][r] = (s16)b;
        }
    }
}

/* HUD line: is the native view (window over plane B) mostly backdrop? */
static int window_line_sparse(int line)
{
    int is40 = h40();
    u32 wbase = is40 ? ((u32)(regs[3] & 0x3C) << 10) : ((u32)(regs[3] & 0x3E) << 10);
    int ww = is40 ? 64 : 32;
    int wc = size_cells(regs[16] & 3), hc = size_cells((regs[16] >> 4) & 3), wpx = wc * 8;
    u32 bbase = plane_base(1);
    int bx0 = (-plane_hscroll(1, line)) & (wpx - 1);
    int by = (line + (vsram[1] & 0x3FF)) & (hc * 8 - 1);
    int tiles = vdp_native_w / 8, used = 0;
    for (int t = 0; t < tiles; t++) {
        int u = tile_row_used(vram_w(wbase + (u32)((line >> 3) * ww + t) * 2), line & 7);
        if (!u) u = tile_row_used(vram_w(bbase + (u32)((by >> 3) * wc + (((bx0 + t * 8) & (wpx - 1)) >> 3)) * 2), by & 7);
        used += u;
    }
    return used * 4 < tiles;
}

static void layout_frame(void)
{
    EMU_STATE static int last_native = -1;
    if (screen_width() != last_native) { rest_vs[0] = rest_vs[1] = 0x7FFF; last_native = screen_width(); }
    for (int p = 0; p < 2; p++) {              /* camera only rises above rest */
        int sv = (s16)(vsram[p] << 5) >> 5;
        if (rest_vs[p] == 0x7FFF || sv > rest_vs[p]) rest_vs[p] = sv;
    }
    vdp_native_w = screen_width();
    int extra = vdp_widescreen ? ((vdp_native_w * 4 / 3 + 1) & ~1) - vdp_native_w : 0;
    out_w = vdp_native_w + extra;
    if (!extra) { vdp_view_x = 0; pad_l = pad_r = 0; return; }

    int availL = extra, availR = extra, any = 0;
    int stage_fill = vdp_hud_rules == 2 && umk3_in_fight();
    plane_fill = stage_fill;
    if (stage_fill) plane_extents();
    hud_hs_line = -1;
    if (regs[1] & 0x40) {
        int dummy_l, dummy_r;
        for (int line = 0; line < active_h; line++) {
            if (window_covers_line(line)) { line_sparse[line] = window_line_sparse(line); continue; }
            if (hud_hs_line < 0) hud_hs_line = line;
            if (line % 6 != 4) { line_extent(line, 0, &dummy_l, &dummy_r); continue; }
            int l, r;
            if (line_extent(line, extra, &l, &r)) {
                any = 1;
                if (vdp_layout_debug) fprintf(stderr, "line %3d  left %3d right %3d\n", line, l, r);
                if (l < availL) availL = l;
                if (r < availR) availR = r;
            }
        }
    }
    if (!any) availL = availR = 0;
    if (vdp_hud_rules == 2) availL = availR = stage_fill ? extra : 0;   /* fights fill, menus centre */
    int half = extra / 2;
    int L = availL < half ? availL : half;
    int R = availR < extra - L ? availR : extra - L;
    if (L + R < extra) L = availL < extra - R ? availL : extra - R;
    /* centre whatever could not be filled as black bars */
    int pad = extra - L - R;
    int npl = pad / 2, npr = pad - npl, nvx = npl + L;
    if (vdp_layout_debug) fprintf(stderr, "LAYOUT availL %d availR %d L %d R %d pad %d hsA %d hsB %d\n", availL, availR, L, R, pad,
                                  plane_hscroll(0, 120), plane_hscroll(1, 120));
    /* Debounce: a big jump (> 6 px) in layout only takes effect when it
     * holds for two frames, which filters the single-frame misreads while
     * the game rewrites VRAM at scene cuts. Small steps follow the camera. */
    EMU_STATE static int last_vx = -1, last_w = -1, cand_vx = -1, cand_pl, cand_pr;
    if (last_w == out_w && last_vx >= 0 && abs(nvx - last_vx) > 6) {
        if (cand_vx != nvx || cand_pl != npl || cand_pr != npr) {
            cand_vx = nvx; cand_pl = npl; cand_pr = npr;
            return;                 /* keep the previous layout for this frame */
        }
    }
    cand_vx = -1;
    pad_l = npl; pad_r = npr; vdp_view_x = nvx;
    last_vx = nvx; last_w = out_w;
}

/* top/bottom overscan mask: the native view is one solid high-priority
 * plane-A/window colour across its whole width */
static int is_mask_band_at(int line, int x0)
{
    if (line >= 16 && line < active_h - 16) return 0;
    const u8 *A = line_a + MARGIN + x0;
    u8 v = A[0];
    if (!(v & 0x80) || !(v & 15)) return 0;
    for (int x = 1; x < vdp_native_w; x++) if (A[x] != v) return 0;
    return 1;
}

static void render_line(int line)
{
    int width = out_w;
    u32 *out = &vdp_framebuffer[line * VDP_FB_STRIDE];
    vdp_screen_w = width;
    int bd = regs[7] & 0x3F;

    if (!(regs[1] & 0x40)) {
        u32 c = pal_n[bd];
        for (int x = 0; x < width; x++) out[x] = (x < pad_l || x >= width - pad_r) ? 0 : c;
        prev_dot_overflow = 0;
        return;
    }

    int hud = width > vdp_native_w && window_covers_line(line);
    render_plane(line_b, 1, line);
    render_plane(line_a, 0, line);
    if (hud) {
        /* HUD rows in widescreen: the window plane (score, health bars) is
         * pinned to the centre of the screen, independent of where the game's
         * view sits; plane A under it holds unrelated data and is dropped */
        int hud_x = (width - vdp_native_w) / 2, save = vdp_view_x;
        memset(line_a, 0, sizeof line_a);
        vdp_view_x = hud_x;
        render_window(line_a, line, vdp_native_w);
        vdp_view_x = save;
        if (is_mask_band_at(line, hud_x)) memset(line_a, 0, sizeof line_a);
    } else render_window(line_a, line, vdp_native_w);
    render_sprites(line);

    const u8 *A = line_a + MARGIN, *B = line_b + MARGIN, *S = line_s;
    int x0 = pad_l, x1 = width - pad_r;
    if (vdp_layout_debug && (line == 44 || line == 50 || line == 56)) {
        fprintf(stderr, "line %d view_x %d hsA %d hsB %d:\n", line, vdp_view_x, plane_hscroll(0, line), plane_hscroll(1, line));
        for (int x = 0; x < width; x += 8)
            fprintf(stderr, "%c%c%c ", (S[x] & 15) ? 'S' : '.', (A[x] & 15) ? 'A' : '.', (B[x] & 15) ? 'B' : '.');
        fprintf(stderr, "\n");
        for (int x = 0; x < width; x += 8) fprintf(stderr, "%02X%02X ", A[x], B[x]);
        fprintf(stderr, "\n");
    }
    if (width > vdp_native_w) {
        int m0 = vdp_view_x, m1 = vdp_view_x + vdp_native_w;   /* native part */
        if (hud) {
            /* behind the HUD the stage continues across the full width,
             * scrolled like the first stage line below it */
            if (hud_hs_line >= 0) render_plane_ex(line_b, 1, line, plane_hscroll(1, hud_hs_line), 1);
            if (line < 16) for (int x = 0; x < width; x++) if (!(line_s[x] & 0x80)) line_s[x] = 0;
        } else if (is_mask_band_at(line, vdp_view_x)) {
            /* The game hides the top/bottom 8 lines behind a row of solid
             * high-priority tiles exactly as wide as its view. In widescreen
             * that reads as black bars, so show the stage there instead. */
            int hs;
            if (line < 16) hs = plane_hscroll(1, hud_hs_line >= 0 ? hud_hs_line : 16);
            else {
                /* continue the floor's per-line perspective scroll linearly */
                int ref = active_h - 17;
                int a = plane_hscroll(1, ref), b = plane_hscroll(1, ref - 8);
                int slope = (((a - b + 512) & 0x3FF) - 512);      /* per 8 lines, wrapped */
                hs = a + slope * (line - ref) / 8;
            }
            render_plane_ex(line_b, 1, line, hs, 1);
            memset(line_a, 0, sizeof line_a);
            /* keep hiding what the mask hid: low-priority sprites parked there */
            for (int x = 0; x < width; x++) if (!(line_s[x] & 0x80)) line_s[x] = 0;
        } else if (line_sparse[line]) {
            for (int x = x0; x < m0; x++) line_a[MARGIN + x] = line_b[MARGIN + x] = 0;
            for (int x = m1; x < x1; x++) line_a[MARGIN + x] = line_b[MARGIN + x] = 0;
        }
    }
    for (int x = 0; x < x0; x++) out[x] = 0;
    for (int x = x1; x < width; x++) out[x] = 0;

#ifdef GENEXIA_OS9                 /* no effects to mask there */
    #define FXMARK(x, s) do { } while (0)
#else
    u8 *fm = &vdp_fx_mask[line * VDP_FB_STRIDE];
    memset(fm, 0, (size_t)width);
    #define FXMARK(x, s) do { int pl_ = ((s) >> 4) & 3; if (pl_ >= 2) fm[x] = (u8)(pl_ - 1); } while (0)
#endif
    if (!(regs[12] & 0x08)) {
#ifdef GENEXIA_OS9
        static u8 mixed[MAXW] __attribute__((aligned(16)));
        if (vdp_altivec && !x0 && !(((size_t)A | (size_t)B | (size_t)S | (size_t)mixed) & 15)) {
            vdp_compose_altivec(mixed, A, B, S, x1);
            const u32 saved = pal_n[0];
            pal_n[0] = pal_n[bd];
            for (int x = 0; x < x1; x++) out[x] = pal_n[mixed[x]];
            pal_n[0] = saved;
            vdp_altivec_lines++;
            return;
        }
#endif
        for (int x = x0; x < x1; x++) {
            u8 a = A[x], b = B[x], s = S[x], c;
            if ((s & 15) && (s & 0x80)) { c = s; FXMARK(x, s); }
            else if ((a & 15) && (a & 0x80)) c = a;
            else if ((b & 15) && (b & 0x80)) c = b;
            else if (s & 15) { c = s; FXMARK(x, s); }
            else if (a & 15) c = a;
            else if (b & 15) c = b;
            else c = (u8)bd;
            out[x] = pal_n[c & 0x3F];
        }
        return;
    }

    /* shadow / highlight */
    for (int x = x0; x < x1; x++) {
        u8 a = A[x], b = B[x], s = S[x];
        int shadow = !((a | b) & 0x80);
        int ahi = (a & 15) && (a & 0x80), bhi = (b & 15) && (b & 0x80);
        u8 pc;  /* plane winner */
        if (ahi) pc = a;
        else if (bhi) pc = b;
        else if (a & 15) pc = a;
        else if (b & 15) pc = b;
        else pc = (u8)bd;
        pc &= 0x3F;

        if (s & 15) {
            int spri = s & 0x80;
            int win = spri || !(ahi || bhi);
            if (win) {
                int sc = s & 0x3F;
                FXMARK(x, s);
                if (sc == 0x3E) { out[x] = shadow ? pal_n[pc] : pal_h[pc]; continue; }
                if (sc == 0x3F) { out[x] = pal_s[pc]; continue; }
                if (spri || (sc & 15) == 14) out[x] = pal_n[sc];
                else out[x] = shadow ? pal_s[sc] : pal_n[sc];
                continue;
            }
        }
        out[x] = shadow ? pal_s[pc] : pal_n[pc];
    }
}

/* ------------------------------------------------------------ timing */

void vdp_begin_line(int line)
{
    cur_line = line;
    if (line == 0) {
        active_h = (regs[1] & 0x08) ? 240 : 224;
        vdp_screen_h = active_h;
        vblank_flag = 0;
        layout_frame();
    }
    if (line < active_h && !vdp_skip_render) render_line(line);

    if (line <= active_h) {
        if (--hcounter < 0) {
            hcounter = regs[10];
            hint_pending = 1;
            irq_update();
        }
    } else {
        hcounter = regs[10];
    }

    if (line == active_h) {
        vblank_flag = 1;
        vint_flag = 1;
        vint_pending = 1;
        irq_update();
    }
}

/* the CPU dispatcher reads vdp_irq_level before every block; it is kept
 * current at every point where pending flags or enable bits change */
EMU_STATE int vdp_irq_level;
static inline void irq_update(void)
{
    vdp_irq_level = (vint_pending && (regs[1] & 0x20)) ? 6 : (hint_pending && (regs[0] & 0x10)) ? 4 : 0;
}

int vdp_irq_pending(void)
{
    irq_update();
    return vdp_irq_level;
}

void vdp_irq_ack(int level)
{
    if (level == 6) { vint_pending = 0; vint_flag = 0; }
    else if (level == 4) hint_pending = 0;
    irq_update();
}

int vdp_zint_pending(void)
{
    return cur_line == active_h;
}

/* ------------------------------------------------------------ debug */
#include <stdio.h>
/* dump whole planes A and B (as laid out in VRAM) plus registers */
void vdp_debug_dump(const char *path)
{
    int wc = size_cells(regs[16] & 3), hc = size_cells((regs[16] >> 4) & 3);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int W = wc * 8, H = hc * 8 * 2;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int p = 0; p < 2; p++) {
        u32 base = p ? ((u32)(regs[4] & 7) << 13) : ((u32)(regs[2] & 0x38) << 10);
        for (int y = 0; y < hc * 8; y++) {
            u8 tmp[1024 + 16];
            for (int tc = 0; tc < wc; tc++)
                draw_tile_row(tmp + tc * 8, vram_w(base + (u32)((y >> 3) * wc + tc) * 2), y & 7);
            for (int x = 0; x < W; x++) {
                u32 c = (tmp[x] & 15) ? pal_n[tmp[x] & 0x3F] : 0x200020;
                u8 rgb[3] = { (u8)(c >> 16), (u8)(c >> 8), (u8)c };
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    fclose(f);
    u32 hsbase = (u32)(regs[13] & 0x3F) << 10;
    fprintf(stderr, "regs:");
    for (int i = 0; i < 24; i++) fprintf(stderr, " %02X", regs[i]);
    fprintf(stderr, "\nplane %dx%d cells, hscroll mode %d, vscroll mode %d, vsram0 %d/%d\n", wc, hc,
            regs[11] & 3, (regs[11] >> 2) & 1, vsram[0], vsram[1]);
    fprintf(stderr, "hscroll A/B per 16 lines:");
    for (int l = 0; l < 224; l += 16)
        fprintf(stderr, " [%d] %d/%d", l, (s16)(vram_w(hsbase + l * 4) << 6) >> 6, (s16)(vram_w(hsbase + l * 4 + 2) << 6) >> 6);
    fprintf(stderr, "\nwindow h=%02X v=%02X\n", regs[17], regs[18]);
}
int vdp_debug_hscroll(int line, int plane_b)
{
    u32 hsbase = (u32)(regs[13] & 0x3F) << 10;
    int hl;
    switch (regs[11] & 3) { case 0: hl = 0; break; case 1: hl = line & 7; break; case 2: hl = line & ~7; break; default: hl = line; }
    return (s16)(vram_w(hsbase + (u32)hl * 4 + (plane_b ? 2 : 0)) << 6) >> 6;
}
int vdp_debug_vscroll(int plane_b) { return vsram[plane_b ? 1 : 0]; }
void vdp_debug_sat(void)
{
    int is40 = h40();
    u32 sat = is40 ? ((u32)(regs[5] & 0x7E) << 9) : ((u32)(regs[5] & 0x7F) << 9);
    int idx = 0;
    for (int n = 0; n < 80; n++) {
        const u8 *s = &vram[(sat + (u32)idx * 8) & 0xFFFF];
        int y = (((s[0] << 8) | s[1]) & 0x1FF) - 128, x = (((s[6] << 8) | s[7]) & 0x1FF) - 128;
        int attr = (s[4] << 8) | s[5];
        fprintf(stderr, "spr %2d  x %4d y %4d  %dx%d  tile %03X pal %d pri %d\n", idx, x, y,
                ((s[2] >> 2) & 3) + 1, (s[2] & 3) + 1, attr & 0x7FF, (attr >> 13) & 3, attr >> 15);
        int link = s[3] & 0x7F;
        if (!link) break;
        idx = link;
    }
}
/* dev: log palette-0 sprites (tile, x) with the camera scroll, one line per frame */
void vdp_debug_pal0(FILE *f, int frame)
{
    int is40 = h40();
    u32 sat = is40 ? ((u32)(regs[5] & 0x7E) << 9) : ((u32)(regs[5] & 0x7F) << 9);
    int idx = 0;
    fprintf(f, "%d %d %d", frame, plane_hscroll(0, 120), vdp_view_x);
    for (int n = 0; n < 80; n++) {
        const u8 *s = &vram[(sat + (u32)idx * 8) & 0xFFFF];
        int x = (((s[6] << 8) | s[7]) & 0x1FF) - 128, attr = (s[4] << 8) | s[5];
        if (getenv("SF2_ALLSPR")) fprintf(f, " %d.%03X:%d", (attr >> 13) & 3, attr & 0x7FF, x);
        else if (((attr >> 13) & 3) == 0) fprintf(f, " %03X:%d", attr & 0x7FF, x);
        int link = s[3] & 0x7F;
        if (!link) break;
        idx = link;
    }
    fprintf(f, "\n");
}
/* dev: bounding box (screen x) of sprites using palette `pal`; returns count */
int vdp_debug_pal_bbox(int pal, int *x0, int *x1, int *y0, int *y1)
{
    int is40 = h40();
    u32 sat = is40 ? ((u32)(regs[5] & 0x7E) << 9) : ((u32)(regs[5] & 0x7F) << 9);
    int idx = 0, n = 0;
    *x0 = *y0 = 9999; *x1 = *y1 = -9999;
    for (int k = 0; k < 80; k++) {
        const u8 *s = &vram[(sat + (u32)idx * 8) & 0xFFFF];
        int y = (((s[0] << 8) | s[1]) & 0x1FF) - 128, x = (((s[6] << 8) | s[7]) & 0x1FF) - 128;
        int attr = (s[4] << 8) | s[5], w = (((s[2] >> 2) & 3) + 1) * 8, h = ((s[2] & 3) + 1) * 8;
        if (((attr >> 13) & 3) == pal && (attr & 0x7FF) >= 0x580) {
            n++;
            if (x < *x0) *x0 = x; if (x + w > *x1) *x1 = x + w;
            if (y < *y0) *y0 = y; if (y + h > *y1) *y1 = y + h;
        }
        int link = s[3] & 0x7F;
        if (!link) break;
        idx = link;
    }
    return n;
}

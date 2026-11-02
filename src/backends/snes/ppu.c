/*
 * ppu.c - SNES S-PPU1/S-PPU2: registers, BG modes 0-7, sprites, windows,
 * colour math. Renders one whole scanline at the start of that line, so
 * register changes made by HDMA before snes_ppu_render_line() apply to it.
 *
 * Line numbering follows the hardware: the first visible line is 1. A BG with
 * VOFS 0 therefore shows its tilemap row 1 on the first visible line, and a
 * sprite with OAM Y = n has its top row on framebuffer row n (it is evaluated
 * on the line before it is drawn).
 *
 * Each layer is first decoded into a line of packed pixels, then merged into
 * the main ("above") and sub ("below") screens by comparing z values, which
 * encode the per-mode priority order. A packed pixel is
 *     bits 0-14 BGR555 colour, bits 16-18 source, bits 24-31 z (0 = none)
 *
 * Hi-res (modes 5/6 and pseudo hi-res) is output at true 512 width. If a frame
 * mixes hi-res and normal lines, the normal lines are pixel-doubled.
 */
#include "snes.h"
#include <string.h>

enum { SRC_BG1, SRC_BG2, SRC_BG3, SRC_BG4, SRC_OBJ_MATH, SRC_BACK, SRC_OBJ_NOMATH };
enum { WIN_OBJ = 4, WIN_COL = 5 };

/* ---------------------------------------------------------- emulated state */
EMU_STATE static u16 vram[0x8000];     /* 64 KB, word addressed */
EMU_STATE static u16 cgram[256];       /* 512 bytes, 15-bit colours */
EMU_STATE static u8  oam[544];

typedef struct {
    u8  inidisp, obsel, bgmode, mosaic, bgsc[4], bgnba[2];
    u16 hofs[4], vofs[4];
    u8  ofs_prev, ofs_prev_h;          /* shared BGnxOFS write latches */
    u8  vmain;
    u16 vmadd, vram_latch;
    u8  m7sel, m7_prev;
    u16 m7a, m7b, m7c, m7d, m7x, m7y, m7hofs, m7vofs;
    u16 cg_addr;
    u8  cg_latch;
    u16 oam_reload, oam_addr;
    u8  oam_latch, oam_prio, oam_first;
    u8  win_sel[3], win_l[2], win_r[2], win_log[2];
    u8  tm, ts, tmw, tsw;
    u8  cgwsel, cgadsub, setini;
    u16 fixed_color;
    u16 lat_h, lat_v;
    u8  lat_flag, ophct_ff, opvct_ff;
    u8  ppu1_mdr, ppu2_mdr;
    u8  time_over, range_over;
    u8  field, in_vblank, frame_hires;
} PpuRegs;
EMU_STATE static PpuRegs P;

/* ------------------------------------------- scratch buffers, lookup tables */
static u32 layer[512];                 /* current layer, packed pixels */
static u32 above[512], below[512];     /* main and sub screen */
static u8  win_mask[6][512];
/* Widescreen: frames with Mode 7 render XO extra pixels on each side (game x
 * -XO..255+XO, buffer index x+XO, LW pixels). Other frames stay 256 wide,
 * centred with black bars (PAD). XO is 0 whenever widescreen is off.
 * vdp_widescreen 2 is ultrawide, 21:9. */
#define WIDE_MARGIN (vdp_widescreen == 2 ? 96 : 43)
static int XO, LW = 256, PAD;
u32 snes_frames_total, snes_frames_mode7;
EMU_STATE static u8 saw_mode7, saw_mode7_prev, wide_frame;
static u8  win_active[6];
static u64 expand_l[256], expand_r[256]; /* bit -> byte; _l: bit 7 is pixel 0 */
static u8  luma[16][32];

static void build_tables(void)
{
    static int ready;
    if (ready) return;
    ready = 1;
    for (int v = 0; v < 256; v++) {
        u64 l = 0, r = 0;
        for (int b = 0; b < 8; b++)
            if (v >> b & 1) {
                l |= (u64)1 << ((7 - b) * 8);
                r |= (u64)1 << (b * 8);
            }
        expand_l[v] = l;
        expand_r[v] = r;
    }
    for (int b = 0; b < 16; b++)
        for (int c = 0; c < 32; c++) {
            int c8 = c << 3 | c >> 2;
            luma[b][c] = b ? (u8)(c8 * (b + 1) / 16) : 0;
        }
}

/* --------------------------------------------------------------- VRAM port */
static inline int vram_accessible(void)
{
    return (P.inidisp & 0x80) || P.in_vblank;
}

static inline u32 vram_remap(void)
{
    u32 a = P.vmadd;
    switch (P.vmain >> 2 & 3) {
    case 1: a = (a & 0xFF00) | (a & 0x001F) << 3 | (a >> 5 & 7); break;
    case 2: a = (a & 0xFE00) | (a & 0x003F) << 3 | (a >> 6 & 7); break;
    case 3: a = (a & 0xFC00) | (a & 0x007F) << 3 | (a >> 7 & 7); break;
    }
    return a & 0x7FFF;
}

static inline u16 vram_fetch(void)
{
    return vram_accessible() ? vram[vram_remap()] : 0;
}

static inline void vram_step(void)
{
    static const u8 inc[4] = { 1, 32, 128, 128 };
    P.vmadd += inc[P.vmain & 3];
}

/* ----------------------------------------------------------------- OAM port */
static void oam_set_first(void)
{
    P.oam_first = P.oam_prio ? (P.oam_addr >> 2) & 0x7F : 0;
}

static void oam_write(u8 v)
{
    u32 a = P.oam_addr;
    P.oam_addr = (a + 1) & 0x3FF;
    if (!(a & 1)) P.oam_latch = v;
    if (a & 0x200) {
        oam[a & 0x21F] = v;
    } else if (a & 1) {
        oam[a - 1] = P.oam_latch;
        oam[a] = v;
    }
}

/* ---------------------------------------------------------------- lifecycle */
void snes_ppu_reset(void)
{
    build_tables();
    memset(vram, 0, sizeof vram);
    memset(cgram, 0, sizeof cgram);
    memset(oam, 0, sizeof oam);
    memset(&P, 0, sizeof P);
    P.inidisp = 0x80;
    P.in_vblank = 1;
    vdp_screen_w = 256;
    vdp_screen_h = 224;
    vdp_native_w = 256;
}

int snes_ppu_overscan(void)     { return P.setini >> 2 & 1; }
int snes_ppu_forced_blank(void) { return P.inidisp >> 7; }

void snes_ppu_vblank(void)
{
    P.in_vblank = 1;
    if (!(P.inidisp & 0x80)) {
        P.oam_addr = P.oam_reload << 1;
        oam_set_first();
    }
}

void snes_ppu_frame_start(void)
{
    saw_mode7_prev = saw_mode7; saw_mode7 = 0;
    snes_frames_total++; snes_frames_mode7 += saw_mode7_prev;
    wide_frame = vdp_widescreen && saw_mode7_prev;
    P.in_vblank = 0;
    P.field ^= 1;
    P.frame_hires = 0;
    P.time_over = P.range_over = 0;
}

void snes_ppu_latch_hv(int h, int v)
{
    P.lat_h = h & 0x1FF;
    P.lat_v = v & 0x1FF;
    P.lat_flag = 1;
}

/* ---------------------------------------------------------------- registers */
void snes_ppu_write(u8 reg, u8 v)
{
    reg &= 0x3F;
    switch (reg & 0x0F) {              /* PPU1 drives these on the data lines */
    case 4: case 5: case 6: case 8: case 9: case 10:
        if (reg < 0x30) P.ppu1_mdr = v;
        break;
    }
    switch (reg) {
    case 0x00: P.inidisp = v; break;
    case 0x01: P.obsel = v; break;
    case 0x02:
        P.oam_reload = (P.oam_reload & 0x100) | v;
        P.oam_addr = P.oam_reload << 1;
        oam_set_first();
        break;
    case 0x03:
        P.oam_reload = (P.oam_reload & 0xFF) | (u16)(v & 1) << 8;
        P.oam_prio = v >> 7;
        P.oam_addr = P.oam_reload << 1;
        oam_set_first();
        break;
    case 0x04: oam_write(v); break;
    case 0x05: P.bgmode = v; break;
    case 0x06: P.mosaic = v; break;
    case 0x07: case 0x08: case 0x09: case 0x0A: P.bgsc[reg - 0x07] = v; break;
    case 0x0B: case 0x0C: P.bgnba[reg - 0x0B] = v; break;

    case 0x0D: case 0x0E: case 0x0F: case 0x10:
    case 0x11: case 0x12: case 0x13: case 0x14: {
        int n = (reg - 0x0D) >> 1;
        if (reg == 0x0D) { P.m7hofs = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; }
        if (reg == 0x0E) { P.m7vofs = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; }
        if ((reg - 0x0D) & 1) {
            P.vofs[n] = ((u16)(v << 8) | P.ofs_prev) & 0x3FF;
            P.ofs_prev = v;
        } else {
            P.hofs[n] = ((u16)(v << 8) | (P.ofs_prev & ~7) | (P.ofs_prev_h & 7)) & 0x3FF;
            P.ofs_prev = P.ofs_prev_h = v;
        }
        break;
    }

    case 0x15: P.vmain = v; break;
    case 0x16:
        P.vmadd = (P.vmadd & 0xFF00) | v;
        P.vram_latch = vram_fetch();
        break;
    case 0x17:
        P.vmadd = (P.vmadd & 0x00FF) | (u16)(v << 8);
        P.vram_latch = vram_fetch();
        break;
    case 0x18:
        if (vram_accessible()) {
            u32 a = vram_remap();
            vram[a] = (vram[a] & 0xFF00) | v;
        }
        if (!(P.vmain & 0x80)) vram_step();
        break;
    case 0x19:
        if (vram_accessible()) {
            u32 a = vram_remap();
            vram[a] = (vram[a] & 0x00FF) | (u16)(v << 8);
        }
        if (P.vmain & 0x80) vram_step();
        break;

    case 0x1A: P.m7sel = v; break;
    case 0x1B: P.m7a = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; break;
    case 0x1C: P.m7b = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; break;
    case 0x1D: P.m7c = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; break;
    case 0x1E: P.m7d = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; break;
    case 0x1F: P.m7x = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; break;
    case 0x20: P.m7y = (u16)(v << 8) | P.m7_prev; P.m7_prev = v; break;

    case 0x21: P.cg_addr = (u16)v << 1; break;
    case 0x22:
        if (!(P.cg_addr & 1)) P.cg_latch = v;
        else cgram[P.cg_addr >> 1] = P.cg_latch | (u16)((v & 0x7F) << 8);
        P.cg_addr = (P.cg_addr + 1) & 0x1FF;
        break;

    case 0x23: case 0x24: case 0x25: P.win_sel[reg - 0x23] = v; break;
    case 0x26: P.win_l[0] = v; break;
    case 0x27: P.win_r[0] = v; break;
    case 0x28: P.win_l[1] = v; break;
    case 0x29: P.win_r[1] = v; break;
    case 0x2A: P.win_log[0] = v; break;
    case 0x2B: P.win_log[1] = v; break;
    case 0x2C: P.tm = v; break;
    case 0x2D: P.ts = v; break;
    case 0x2E: P.tmw = v; break;
    case 0x2F: P.tsw = v; break;
    case 0x30: P.cgwsel = v; break;
    case 0x31: P.cgadsub = v; break;
    case 0x32:
        if (v & 0x20) P.fixed_color = (P.fixed_color & ~0x001F) | (v & 31);
        if (v & 0x40) P.fixed_color = (P.fixed_color & ~0x03E0) | (u16)(v & 31) << 5;
        if (v & 0x80) P.fixed_color = (P.fixed_color & ~0x7C00) | (u16)(v & 31) << 10;
        break;
    case 0x33: P.setini = v; break;
    default: break;
    }
}

u8 snes_ppu_read(u8 reg)
{
    u8 v;
    reg &= 0x3F;
    switch (reg) {
    case 0x34: case 0x35: case 0x36: {
        s32 r = (s32)(s16)P.m7a * (s32)(s8)(P.m7b >> 8);
        return P.ppu1_mdr = (u8)((u32)r >> ((reg - 0x34) * 8));
    }
    case 0x38: {
        u32 a = P.oam_addr;
        P.oam_addr = (a + 1) & 0x3FF;
        return P.ppu1_mdr = (a & 0x200) ? oam[a & 0x21F] : oam[a];
    }
    case 0x39:
        v = (u8)P.vram_latch;
        if (!(P.vmain & 0x80)) { P.vram_latch = vram_fetch(); vram_step(); }
        return P.ppu1_mdr = v;
    case 0x3A:
        v = (u8)(P.vram_latch >> 8);
        if (P.vmain & 0x80) { P.vram_latch = vram_fetch(); vram_step(); }
        return P.ppu1_mdr = v;
    case 0x3B: {
        u16 c = cgram[P.cg_addr >> 1];
        if (!(P.cg_addr & 1)) v = (u8)c;
        else v = (P.ppu2_mdr & 0x80) | (u8)(c >> 8);
        P.cg_addr = (P.cg_addr + 1) & 0x1FF;
        return P.ppu2_mdr = v;
    }
    case 0x3C:
        v = P.ophct_ff ? (P.ppu2_mdr & 0xFE) | (P.lat_h >> 8 & 1) : (u8)P.lat_h;
        P.ophct_ff ^= 1;
        return P.ppu2_mdr = v;
    case 0x3D:
        v = P.opvct_ff ? (P.ppu2_mdr & 0xFE) | (P.lat_v >> 8 & 1) : (u8)P.lat_v;
        P.opvct_ff ^= 1;
        return P.ppu2_mdr = v;
    case 0x3E:
        v = (P.ppu1_mdr & 0x10) | (u8)(P.time_over << 7) | (u8)(P.range_over << 6) | 1;
        return P.ppu1_mdr = v;
    case 0x3F:
        v = (P.ppu2_mdr & 0x20) | (u8)(P.field << 7) | (u8)(P.lat_flag << 6) | 3;
        P.lat_flag = 0;
        P.ophct_ff = P.opvct_ff = 0;
        return P.ppu2_mdr = v;
    default:
        return P.ppu1_mdr;             /* $2137 and write-only registers */
    }
}

/* ------------------------------------------------------------------ windows */
static void window_range(u8 *o, int n, u8 invert)
{
    int l = P.win_l[n], r = P.win_r[n];
    memset(o, invert, 256);
    if (l <= r) memset(o + l, !invert, (size_t)(r - l + 1));
}

/* Computes win_mask[li] (1 = inside the combined window). Returns 0 when the
 * layer has no window enabled; the mask is then all zero. */
static int window_build_native(int li);
static int window_build(int li)
{
    int r = window_build_native(li);
    if (XO) {                  /* margins continue the window state at each edge */
        u8 *o = win_mask[li];
        memmove(o + XO, o, 256);
        memset(o, o[XO], (size_t)XO);
        memset(o + XO + 256, o[XO + 255], (size_t)XO);
    }
    return r;
}
static int window_build_native(int li)
{
    u8 *o = win_mask[li];
    int nibble = P.win_sel[li >> 1] >> ((li & 1) * 4) & 15;
    int en1 = nibble >> 1 & 1, en2 = nibble >> 3 & 1;
    if (!en1 && !en2) { memset(o, 0, 256); return 0; }
    if (en1 && !en2) { window_range(o, 0, nibble & 1); return 1; }
    if (en2 && !en1) { window_range(o, 1, nibble >> 2 & 1); return 1; }

    static u8 b[256];
    int logic = (li < 4 ? P.win_log[0] >> (li * 2) : P.win_log[1] >> ((li - 4) * 2)) & 3;
    window_range(o, 0, nibble & 1);
    window_range(b, 1, nibble >> 2 & 1);
    switch (logic) {
    case 0: for (int x = 0; x < 256; x++) o[x] |= b[x]; break;
    case 1: for (int x = 0; x < 256; x++) o[x] &= b[x]; break;
    case 2: for (int x = 0; x < 256; x++) o[x] ^= b[x]; break;
    case 3: for (int x = 0; x < 256; x++) o[x] = !(o[x] ^ b[x]); break;
    }
    return 1;
}

/* ------------------------------------------------------- merging a layer in */
static inline void clear_layer(int width)
{
    memset(layer, 0, (size_t)width * sizeof layer[0]);
}

/* Merges `layer` into the main/sub screens. width 512 = hi-res BG: odd dots
 * belong to the main screen, even dots to the sub screen. */
static void merge_layer(int li, int width, int mos)
{
    int main_on = P.tm >> li & 1, sub_on = P.ts >> li & 1;
    int main_win = (P.tmw >> li & 1) && win_active[li];
    int sub_win  = (P.tsw >> li & 1) && win_active[li];
    const u8 *wm = win_mask[li];

    if (mos > 1) {
        if (width == 512) mos *= 2;
        u32 held = 0;
        int mw = width == 256 ? LW : width;
        for (int x = 0; x < mw; x++) {
            if (x % mos == 0) held = layer[x];
            else layer[x] = held;
        }
    }

    if (width == 256) {
        if (!main_win && !sub_win) {       /* the usual case: an empty pixel has z 0 and never wins */
            if (main_on)
                for (int x = 0; x < LW; x++) { u32 p = layer[x]; if (p >> 24 > above[x] >> 24) above[x] = p; }
            if (sub_on)
                for (int x = 0; x < LW; x++) { u32 p = layer[x]; if (p >> 24 > below[x] >> 24) below[x] = p; }
            return;
        }
        for (int x = 0; x < LW; x++) {
            u32 p = layer[x];
            if (!p) continue;
            if (main_on && p >> 24 > above[x] >> 24 && !(main_win && wm[x])) above[x] = p;
            if (sub_on && p >> 24 > below[x] >> 24 && !(sub_win && wm[x])) below[x] = p;
        }
    } else {
        for (int x = 0; x < 512; x++) {
            u32 p = layer[x];
            int s = x >> 1;
            if (!p) continue;
            if (x & 1) {
                if (main_on && p >> 24 > above[s] >> 24 && !(main_win && wm[s])) above[s] = p;
            } else {
                if (sub_on && p >> 24 > below[s] >> 24 && !(sub_win && wm[s])) below[s] = p;
            }
        }
    }
}

static inline int mosaic_size(int n)
{
    return (P.mosaic >> n & 1) ? (P.mosaic >> 4) + 1 : 1;
}

static inline int mosaic_line(int line, int mos)
{
    return line - (line - 1) % mos;
}

static inline u16 direct_color(u32 pal, u32 c)
{
    return (u16)((c << 2 & 0x001C) + (pal << 1 & 0x0002) +
                 (c << 4 & 0x0380) + (pal << 5 & 0x0040) +
                 (c << 7 & 0x6000) + (pal << 10 & 0x1000));
}

/* ---------------------------------------------------------- tiled BG layers */
static inline u32 bg_map_entry(int n, u32 ho, u32 vo, int hires)
{
    u32 sc = P.bgsc[n];
    int tsize = P.bgmode >> (4 + n) & 1;
    int th = 3 + tsize, tw = hires ? 4 : th;
    u32 tx = ho >> tw, ty = vo >> th;
    u32 off = (ty & 0x1F) << 5 | (tx & 0x1F);
    if ((tx & 0x20) && (sc & 1)) off += 32u << 5;
    if ((ty & 0x20) && (sc & 2)) off += 32u << (5 + (sc & 1));
    return vram[(((sc & 0xFC) << 8) + off) & 0x7FFF];
}

static void render_bg(int n, int line, int bpp, u32 pal_base, u32 z0, u32 z1, int hires, int opt)
{
    if (!((P.tm | P.ts) >> n & 1)) return;
    int width = hires ? 512 : 256;
    clear_layer(hires ? width : LW);
    win_active[n] = ((P.tmw | P.tsw) >> n & 1) ? window_build(n) : 0;

    int mos = mosaic_size(n);
    int y = mos > 1 ? mosaic_line(line, mos) : line;
    u32 sc = P.bgsc[n];
    int tsize = P.bgmode >> (4 + n) & 1;
    int th = 3 + tsize, tw = hires ? 4 : th;
    u32 hmask = ((u32)width << tsize << (sc & 1)) - 1;
    u32 vmask = (256u << tsize << (sc >> 1 & 1)) - 1;
    u32 hs = P.hofs[n], vs = P.vofs[n];
    if (hires) {
        hs <<= 1;
        if (P.setini & 1) y = y << 1 | P.field;
    }
    u32 chr = (u32)(P.bgnba[n >> 1] >> ((n & 1) * 4) & 15) << 12;
    int tile_shift = bpp == 2 ? 3 : bpp == 4 ? 4 : 5;
    int pal_shift = bpp == 2 ? 2 : 4;
    int direct = bpp == 8 && (P.cgwsel & 1);
    u32 valid_bit = 0x2000u << n;
    u32 src = (u32)n << 16;

    for (int x = -(int)(hs & 7) - ((XO + 7) & ~7); x < width + XO; x += 8) {
        u32 ho = (u32)(x + (int)hs), vo = (u32)y + vs;
        if (opt) {
            int ox = x + (int)(hs & 7);
            if (ox >= 8) {
                u32 bx = (u32)(ox - 8) + (P.hofs[2] & ~7u);
                u32 hl = bg_map_entry(2, bx & 0xFFFF, P.vofs[2] & 0xFFFF, hires);
                if ((P.bgmode & 7) == 4) {
                    if (hl & valid_bit) {
                        if (!(hl & 0x8000)) ho = (u32)ox + (hl & ~7u);
                        else vo = (u32)y + hl;
                    }
                } else {
                    u32 vl = bg_map_entry(2, bx & 0xFFFF, (P.vofs[2] + 8) & 0xFFFF, hires);
                    if (hl & valid_bit) ho = (u32)ox + (hl & ~7u);
                    if (vl & valid_bit) vo = (u32)y + vl;
                }
            }
        }
        ho &= hmask;
        vo &= vmask;

        u32 e = bg_map_entry(n, ho, vo, hires);
        int vf = e >> 15 & 1, hf = e >> 14 & 1;
        u32 tn = e & 0x3FF;
        if (tw == 4 && (int)(ho >> 3 & 1) != hf) tn += 1;
        if (th == 4 && (int)(vo >> 3 & 1) != vf) tn += 16;
        tn &= 0x3FF;
        u32 row_y = vo & 7;
        if (vf) row_y ^= 7;
        u32 a = (chr + (tn << tile_shift) + row_y) & 0x7FFF;

        const u64 *tab = hf ? expand_r : expand_l;
        u32 w = vram[a];
        u64 row = tab[w & 0xFF] | tab[w >> 8] << 1;
        if (bpp >= 4) {
            w = vram[(a + 8) & 0x7FFF];
            row |= tab[w & 0xFF] << 2 | tab[w >> 8] << 3;
        }
        if (bpp == 8) {
            w = vram[(a + 16) & 0x7FFF];
            row |= tab[w & 0xFF] << 4 | tab[w >> 8] << 5;
            w = vram[(a + 24) & 0x7FFF];
            row |= tab[w & 0xFF] << 6 | tab[w >> 8] << 7;
        }
        if (!row) continue;

        u32 top = ((e & 0x2000) ? z1 : z0) << 24 | src;
        u32 palno = e >> 10 & 7;
        u32 pal = pal_base + (palno << pal_shift);
        int i0 = x + XO < 0 ? -(x + XO) : 0;
        int i1 = width + XO - x < 8 ? width + XO - x : 8;
        for (int i = i0; i < i1; i++) {
            u32 px = (u32)(row >> (i * 8)) & 0xFF;
            if (!px) continue;
            if (direct)        layer[x + XO + i] = top | direct_color(palno, px);
            else if (bpp == 8) layer[x + XO + i] = top | cgram[px];
            else               layer[x + XO + i] = top | cgram[(pal + px) & 0xFF];
        }
    }
    merge_layer(n, width, mos);
}

/* ------------------------------------------------------------------- Mode 7 */
static inline s32 sext13(u32 v) { return (s32)(v << 19) >> 19; }
static inline s32 m7_clip(s32 n) { return (n & 0x2000) ? (n | ~1023) : (n & 1023); }

static void render_mode7(int n, int line, u32 z0, u32 z1)
{
    if (!((P.tm | P.ts) >> n & 1)) return;
    win_active[n] = ((P.tmw | P.tsw) >> n & 1) ? window_build(n) : 0;

    int mos = mosaic_size(n);
    int yy = mos > 1 ? mosaic_line(line, mos) : line;
    int y = (P.m7sel & 2) ? 255 - yy : yy;
    int hflip = P.m7sel & 1, rep = P.m7sel >> 6;
    s32 a = (s16)P.m7a, b = (s16)P.m7b, c = (s16)P.m7c, d = (s16)P.m7d;
    s32 cx = sext13(P.m7x), cy = sext13(P.m7y);
    s32 hx = m7_clip(sext13(P.m7hofs) - cx), vy = m7_clip(sext13(P.m7vofs) - cy);
    s32 ox = (a * hx & ~63) + (b * vy & ~63) + (b * y & ~63) + (s32)((u32)cx << 8);
    s32 oy = (c * hx & ~63) + (d * vy & ~63) + (d * y & ~63) + (s32)((u32)cy << 8);
    int direct = n == 0 && (P.cgwsel & 1);
    u32 src = (u32)n << 16;

    /* the position advances by a constant, no multiply per pixel */
    int x0 = hflip ? 255 + XO : -XO;
    s32 fx = ox + a * x0, fy = oy + c * x0, dx = hflip ? -a : a, dy = hflip ? -c : c;
    /* First layer of a plain line: every pixel beats the backdrop, so it can go
     * straight to the screens. Wrapping maps need no outside test. */
    if (n == 0 && mos == 1 && !direct && rep < 2 &&
        !(((P.tmw | P.tsw) & 1) && win_active[0])) {
        const u32 top = z0 << 24 | src;
        const int main_on = P.tm & 1, sub_on = P.ts & 1;
        for (int i = 0; i < LW; i++, fx += dx, fy += dy) {
            u32 t = vram[((u32)fy >> 11 & 127) << 7 | ((u32)fx >> 11 & 127)] & 0xFF;
            u32 pix = vram[t << 6 | ((u32)fy >> 8 & 7) << 3 | ((u32)fx >> 8 & 7)] >> 8;
            if (!pix) continue;
            u32 p = top | cgram[pix];
            if (main_on) above[i] = p;
            if (sub_on) below[i] = p;
        }
        return;
    }
    clear_layer(LW);
    for (int sx = -XO; sx < 256 + XO; sx++, fx += dx, fy += dy) {
        s32 px = fx >> 8, py = fy >> 8;
        int outside = ((px | py) & ~1023) != 0;
        u32 t;
        if (outside && rep == 2) continue;
        px &= 1023;
        py &= 1023;
        if (outside && rep == 3) t = 0;
        else t = vram[(u32)(py >> 3) << 7 | (u32)(px >> 3)] & 0xFF;
        u32 pix = vram[t << 6 | (u32)(py & 7) << 3 | (u32)(px & 7)] >> 8;
        u32 z = z0;
        if (n == 1) {
            if (pix & 0x80) z = z1;
            pix &= 0x7F;
        }
        if (!pix) continue;
        layer[sx + XO] = z << 24 | src | (direct ? direct_color(0, pix) : cgram[pix]);
    }
    merge_layer(n, 256, mos);
}

/* ------------------------------------------------------------------ sprites */
static const u8 obj_sizes[8][4] = {   /* small w,h, large w,h */
    {  8,  8, 16, 16 }, {  8,  8, 32, 32 }, {  8,  8, 64, 64 }, { 16, 16, 32, 32 },
    { 16, 16, 64, 64 }, { 32, 32, 64, 64 }, { 16, 32, 32, 64 }, { 16, 32, 32, 32 },
};

/* Range and time evaluation; when `draw`, also fills `layer`. The 32 sprites
 * in range are fetched last to first, so when the 34-tile budget runs out it
 * is the earliest sprites that lose tiles, and earlier sprites overwrite later
 * ones in the line buffer. Returns the number of sprites in range. */
static int render_obj_line(int line, int draw, const u32 *z)
{
    const u8 *size = obj_sizes[P.obsel >> 5];
    int interlace = P.setini >> 1 & 1;
    u32 cy = (u32)(line - 1);
    u8 list[128];
    int count = 0, max_list = vdp_no_sprite_limit ? 128 : 32, max_tiles = vdp_no_sprite_limit ? 1 << 20 : 34;

    for (int i = 0; i < 128; i++) {
        int n = (P.oam_first + i) & 127;
        const u8 *o = oam + n * 4;
        int hi = oam[512 + (n >> 2)] >> ((n & 3) * 2) & 3;
        int w = size[(hi >> 1) * 2], h = size[(hi >> 1) * 2 + 1];
        if (((cy - o[1]) & 0xFF) >= (u32)(h >> interlace)) continue;
        int x = o[0] | (hi & 1) << 8;
        if (x > 256 && x + w - 1 < 512 - XO) continue;   /* parked past 256: stay hidden */
        if (count == max_list) { P.range_over = 1; break; }
        list[count++] = (u8)n;
    }
    if (!count) return 0;

    u32 base = (u32)(P.obsel & 7) << 13;
    u32 gap = ((u32)(P.obsel >> 3 & 3) + 1) << 12;
    if (draw) clear_layer(LW);

    int tiles = 0;
    for (int i = count - 1; i >= 0; i--) {
        int n = list[i];
        const u8 *o = oam + n * 4;
        int hi = oam[512 + (n >> 2)] >> ((n & 3) * 2) & 3;
        int w = size[(hi >> 1) * 2], h = size[(hi >> 1) * 2 + 1];
        int x = o[0] | (hi & 1) << 8;
        u32 attr = o[3];
        int vf = attr >> 7 & 1, hf = attr >> 6 & 1;
        int ry = (int)((cy - o[1]) & 0xFF);
        if (interlace) ry = ry << 1 | P.field;
        if (vf) {
            if (w == h) ry = h - 1 - ry;
            else if (ry < w) ry = w - 1 - ry;
            else ry = w + (w - 1 - (ry - w));
        }
        u32 chr = attr & 1 ? base + gap : base;
        u32 cx = o[2] & 15, cr = (u32)(o[2] >> 4) + (u32)(ry >> 3);
        u32 pal = 128 + ((attr >> 1 & 7) << 4);
        u32 top = z[attr >> 4 & 3] << 24 |
                  (u32)(pal >= 192 ? SRC_OBJ_MATH : SRC_OBJ_NOMATH) << 16;
        const u64 *tab = hf ? expand_r : expand_l;
        int tw = w >> 3;

        for (int tx = 0; tx < tw; tx++) {
            int sx = (x + tx * 8) & 511;
            if (x != 256 && sx >= 256 && sx + 7 < 512 - XO) continue;
            if (tiles++ >= max_tiles) { P.time_over = 1; break; }
            if (!draw) continue;
            u32 mx = (u32)(hf ? tw - 1 - tx : tx);
            u32 tile = ((cx + mx) & 15) | (cr & 15) << 4;
            u32 a = (chr + (tile << 4) + (u32)(ry & 7)) & 0x7FFF;
            u32 w0 = vram[a], w1 = vram[(a + 8) & 0x7FFF];
            u64 row = tab[w0 & 0xFF] | tab[w0 >> 8] << 1 | tab[w1 & 0xFF] << 2 | tab[w1 >> 8] << 3;
            if (!row) continue;
            int px = sx >= 256 ? sx - 512 : sx;
            for (int b = 0; b < 8; b++, row >>= 8) {
                int dx = px + b + XO;
                u32 c = (u32)row & 15;
                if (c && dx >= 0 && dx < LW) layer[dx] = top | cgram[pal + c];
            }
        }
    }
    return count;
}

/* --------------------------------------------------------------- compositing */
static inline u16 color_math(u32 x, u32 y, int halve)
{
    if (!(P.cgadsub & 0x80)) {
        if (!halve) {
            u32 sum = x + y;
            u32 carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
            return (u16)((sum - carry) | (carry - (carry >> 5)));
        }
        return (u16)((x + y - ((x ^ y) & 0x0421)) >> 1);
    }
    u32 diff = x - y + 0x8420;
    u32 borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
    u32 r = (diff - borrow) & (borrow - (borrow >> 5));
    return (u16)(halve ? (r & 0x7BDE) >> 1 : r);
}

static inline u16 math_pixel(u32 a, u32 b, int keep, int allow)
{
    u32 col = keep ? a & 0x7FFF : 0;
    if (!allow || !((P.cgadsub & 0x3F) >> (a >> 16 & 7) & 1)) return (u16)col;
    int halve = (P.cgadsub & 0x40) && keep;
    if (!(P.cgwsel & 2)) return color_math(col, P.fixed_color, halve);
    return color_math(col, b & 0x7FFF, halve && (b >> 16 & 7) != SRC_BACK);
}

static inline u32 to_rgb(const u8 *t, u32 c)
{
    return (u32)t[c & 31] << 16 | (u32)t[c >> 5 & 31] << 8 | t[c >> 10 & 31];
}

static inline int region_on(int mode, int inside)
{
    return mode == 0 ? 1 : mode == 1 ? inside : mode == 2 ? !inside : 0;
}

static void compose_line(u32 *out, int hires)
{
    const u8 *t = luma[P.inidisp & 15];
    int clip_mode = P.cgwsel >> 6 & 3, math_mode = P.cgwsel >> 4 & 3;

    if (!(clip_mode == 1 || clip_mode == 2 || math_mode == 1 || math_mode == 2) ||
        !window_build(WIN_COL))
        memset(win_mask[WIN_COL], 0, (size_t)LW);
    const u8 *cw = win_mask[WIN_COL];

    if (!hires) {
        if (!clip_mode && !(P.cgadsub & 0x3F)) {
            for (int x = 0; x < LW; x++) out[x] = to_rgb(t, above[x] & 0x7FFF);
        } else if (!clip_mode && !math_mode) {   /* no colour window: only the layer decides */
            const u32 layers = P.cgadsub & 0x3F;
            for (int x = 0; x < LW; x++) {
                const u32 a = above[x];
                out[x] = to_rgb(t, layers >> (a >> 16 & 7) & 1 ? math_pixel(a, below[x], 1, 1) : a & 0x7FFF);
            }
        } else {
            for (int x = 0; x < LW; x++) {
                int keep = region_on(clip_mode, cw[x]), allow = region_on(math_mode, cw[x]);
                out[x] = to_rgb(t, math_pixel(above[x], below[x], keep, allow));
            }
        }
        if (P.frame_hires)
            for (int x = 255; x >= 0; x--) out[x * 2] = out[x * 2 + 1] = out[x];
        return;
    }
    for (int x = 0; x < 256; x++) {
        int keep = region_on(clip_mode, cw[x]), allow = region_on(math_mode, cw[x]);
        out[x * 2]     = to_rgb(t, math_pixel(below[x], above[x], keep, allow));
        out[x * 2 + 1] = to_rgb(t, math_pixel(above[x], below[x], keep, allow));
    }
}

/* Turns the lines drawn so far this frame into doubled-width lines. */
static void widen_previous_lines(int lines)
{
    for (int l = 0; l < lines; l++) {
        u32 *p = vdp_framebuffer + l * VDP_FB_STRIDE;
        for (int x = 255; x >= 0; x--) p[x * 2] = p[x * 2 + 1] = p[x];
    }
}

void snes_ppu_render_line(int line)
{
    if (line < 1 || line > 240) return;
    int mode = P.bgmode & 7;
    int bg_hires = mode == 5 || mode == 6;
    int hires = bg_hires || (P.setini & 8);
    int blank = P.inidisp >> 7;

    if (hires && !P.frame_hires) {
        P.frame_hires = 1;
        if (!vdp_skip_render) widen_previous_lines(line - 1);
    }
    if (mode == 7 && !blank) saw_mode7 = 1;
    int wide_out = vdp_widescreen && !P.frame_hires;
    XO = wide_out && wide_frame ? WIDE_MARGIN : 0;
    LW = 256 + 2 * XO;
    PAD = wide_out && !wide_frame ? WIDE_MARGIN : 0;
    vdp_screen_w = P.frame_hires ? 512 : wide_out ? 256 + 2 * WIDE_MARGIN : 256;
    vdp_screen_h = (P.setini & 4) ? 239 : 224;
    vdp_native_w = P.frame_hires ? 512 : 256;

    if (vdp_skip_render) {
        static const u32 none[4];
        if (!blank) render_obj_line(line, 0, none);
        return;
    }

    u32 *out = vdp_framebuffer + (line - 1) * VDP_FB_STRIDE;
    if (PAD) {                     /* not a Mode 7 frame: centred, black sides */
        memset(out, 0, (size_t)PAD * sizeof *out);
        memset(out + PAD + 256, 0, (size_t)PAD * sizeof *out);
        out += PAD;
    }
    if (blank) {
        memset(out, 0, (P.frame_hires ? 512u : (size_t)LW) * sizeof *out);
        return;
    }

    u32 a0 = 5u << 16 | cgram[0];
    u32 b0 = 5u << 16 | (hires ? cgram[0] : P.fixed_color);
    for (int x = 0; x < LW; x++) { above[x] = a0; below[x] = b0; }
    memset(win_active, 0, sizeof win_active);

    int bg3p = P.bgmode & 8;
    int ext = P.setini & 0x40;
    u32 oz[4];
#define OZ(a, b, c, d) (oz[0] = (a), oz[1] = (b), oz[2] = (c), oz[3] = (d))
    switch (mode) {
    case 0:
        OZ(3, 6, 9, 12);
        render_bg(0, line, 2,  0, 8, 11, 0, 0);
        render_bg(1, line, 2, 32, 7, 10, 0, 0);
        render_bg(2, line, 2, 64, 2,  5, 0, 0);
        render_bg(3, line, 2, 96, 1,  4, 0, 0);
        break;
    case 1:
        if (bg3p) {
            OZ(2, 3, 6, 9);
            render_bg(0, line, 4, 0, 5, 8, 0, 0);
            render_bg(1, line, 4, 0, 4, 7, 0, 0);
            render_bg(2, line, 2, 0, 1, 10, 0, 0);
        } else {
            OZ(2, 4, 7, 10);
            render_bg(0, line, 4, 0, 6, 9, 0, 0);
            render_bg(1, line, 4, 0, 5, 8, 0, 0);
            render_bg(2, line, 2, 0, 1, 3, 0, 0);
        }
        break;
    case 2:
        OZ(2, 4, 6, 8);
        render_bg(0, line, 4, 0, 3, 7, 0, 1);
        render_bg(1, line, 4, 0, 1, 5, 0, 1);
        break;
    case 3:
        OZ(2, 4, 6, 8);
        render_bg(0, line, 8, 0, 3, 7, 0, 0);
        render_bg(1, line, 4, 0, 1, 5, 0, 0);
        break;
    case 4:
        OZ(2, 4, 6, 8);
        render_bg(0, line, 8, 0, 3, 7, 0, 1);
        render_bg(1, line, 2, 0, 1, 5, 0, 1);
        break;
    case 5:
        OZ(2, 4, 6, 8);
        render_bg(0, line, 4, 0, 3, 7, 1, 0);
        render_bg(1, line, 2, 0, 1, 5, 1, 0);
        break;
    case 6:
        OZ(1, 3, 4, 6);
        render_bg(0, line, 4, 0, 2, 5, 1, 1);
        break;
    default:
        if (ext) {
            OZ(2, 4, 6, 7);
            render_mode7(0, line, 3, 3);
            render_mode7(1, line, 1, 5);
        } else {
            OZ(1, 3, 4, 5);
            render_mode7(0, line, 2, 2);
        }
        break;
    }
#undef OZ

    if (render_obj_line(line, 1, oz) && ((P.tm | P.ts) & 0x10)) {
        win_active[WIN_OBJ] = ((P.tmw | P.tsw) & 0x10) ? window_build(WIN_OBJ) : 0;
        merge_layer(WIN_OBJ, 256, 1);
    }
    compose_line(out, hires);
    if (XO) {
        /* a line the game blanks inside its own view stays blank at the sides */
        u32 c = out[XO];
        int same = 1;
        for (int x = XO + 1; x < XO + 256 && same; x++) same = out[x] == c;
        if (same && !c) {
            for (int x = 0; x < XO; x++) out[x] = out[XO + 256 + x] = c;
        }
    }
}

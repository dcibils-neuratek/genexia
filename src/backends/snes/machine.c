/*
 * machine.c - the SNES itself: memory map, DMA/HDMA, timers, interrupts,
 * multiply/divide unit, joypads and the scanline scheduler. The chips (65C816,
 * PPU, APU, DSP-1) live behind their own interfaces.
 */
#include <string.h>
#include "snes.h"
#include "machine.h"
#include "w65816.h"
#include "m68k.h"     /* rom[] / rom_size */
#include "bus.h"      /* pad_state */

/* ------------------------------------------------------------- state */
EMU_STATE static u8 wram[0x20000];
EMU_STATE static u8 sram[0x20000];
EMU_STATE static u32 wmadd;
EMU_STATE s32 snes_master;
EMU_STATE s32 snes_slice_end;
EMU_STATE int snes_rom_speed = 8;   /* master cycles per fast-ROM-area access */
EMU_STATE static s32 line_start;
EMU_STATE static int cur_line;
EMU_STATE static u8 nmitimen, memsel, wrmpya, rdio;
EMU_STATE u8 snes_mdr;
#define mdr snes_mdr
EMU_STATE static u16 wrdiv, rddiv, rdmpy, htime, vtime;
EMU_STATE static u8 nmi_flag, irq_flag, in_vblank, joy_busy;
EMU_STATE static s32 irq_at;
EMU_STATE static u16 joy_auto[2];
EMU_STATE static u8 joy_strobe;
EMU_STATE static u16 joy_shift[2];

typedef struct {
    u8 dmap, bbad, a1b, dasb, ntrl, unused, done, do_transfer;
    u16 a1t, das, a2a;
} Chan;
EMU_STATE static Chan ch[8];
EMU_STATE static u8 hdmaen;


static const u8 dma_pat[8][4] = {
    {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,0,1,1},{0,1,2,3},{0,1,0,1},{0,0,0,0},{0,0,1,1}
};
static const u8 dma_len[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };

/* --------------------------------------------------- cartridge mapping */
static int hirom, has_dsp1;
static u32 sram_mask;
static inline u32 rom_at(u32 i) {
    return rom[rom_size ? (i < rom_size ? i : i % rom_size) : 0];
}
const u8 *snes_rmap[4096];
u8 *snes_wmap[4096];
u8 snes_tmap[4096];

/* ROM offset of a cartridge address, -1 for anything that isn't ROM */
static s32 rom_index(u32 a) {
    u32 bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
    if (hirom) {
        if (has_dsp1 && b < 0x20 && off >= 0x6000 && off < 0x8000) return -1;
        if (b >= 0x20 && b < 0x40 && off >= 0x6000 && off < 0x8000 && sram_mask) return -1;
        if ((bank >= 0x40 && bank < 0x7E) || bank >= 0xC0) return (s32)(a & 0x3FFFFF);
        if (off >= 0x8000) return (s32)((b << 16 | off) & 0x3FFFFF);
    } else {
        if (has_dsp1 && b >= 0x30 && b < 0x40 && off >= 0x8000) return -1;
        if (b >= 0x70 && b < 0x7E && off < 0x8000 && sram_mask) return -1;
        if (off >= 0x8000 && !(bank >= 0x7E && bank < 0x80)) return (s32)(b << 15 | (off & 0x7FFF));
    }
    return -1;
}

/* returns 1 and sets *v for cartridge addresses */
static int cart_read(u32 a, u8 *v) {
    u32 bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
    s32 i = rom_index(a);
    if (i >= 0) { *v = (u8)rom_at((u32)i); return 1; }
    if (hirom) {
        if (has_dsp1 && b < 0x20 && off >= 0x6000 && off < 0x8000) { *v = snes_dsp1_read(off >= 0x7000); return 1; }
        if (b >= 0x20 && b < 0x40 && off >= 0x6000 && off < 0x8000 && sram_mask) {
            *v = sram[(((b - 0x20) << 13) | (off - 0x6000)) & sram_mask]; return 1;
        }
    } else {
        if (has_dsp1 && b >= 0x30 && b < 0x40 && off >= 0x8000) { *v = snes_dsp1_read(off >= 0xC000); return 1; }
        if (b >= 0x70 && b < 0x7E && off < 0x8000 && sram_mask) {
            *v = sram[(((b - 0x70) << 15) | off) & sram_mask]; return 1;
        }
    }
    return 0;
}
static int cart_write(u32 a, u8 v) {
    u32 bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
    if (hirom) {
        if (has_dsp1 && b < 0x20 && off >= 0x6000 && off < 0x7000) { snes_dsp1_write(v); return 1; }
        if (b >= 0x20 && b < 0x40 && off >= 0x6000 && off < 0x8000 && sram_mask) {
            sram[(((b - 0x20) << 13) | (off - 0x6000)) & sram_mask] = v; return 1;
        }
    } else {
        if (has_dsp1 && b >= 0x30 && b < 0x40 && off >= 0x8000 && off < 0xC000) { snes_dsp1_write(v); return 1; }
        if (b >= 0x70 && b < 0x7E && off < 0x8000 && sram_mask) {
            sram[(((b - 0x70) << 15) | off) & sram_mask] = v; return 1;
        }
    }
    return 0;
}

#ifdef GENEXIA_OS9
u32 snes_t_ppu, snes_t_apu;
static inline u32 now(void) { u32 t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
static void apu_catch_up(s32 m) { const u32 t = now(); snes_apu_run_to(m); snes_t_apu += now() - t; }
static void ppu_line(int line) { const u32 t = now(); snes_ppu_render_line(line); snes_t_ppu += now() - t; }
#else
#define apu_catch_up snes_apu_run_to
#define ppu_line snes_ppu_render_line
#endif

/* ------------------------------------------------------ B bus ($21xx) */
static u8 bbus_read(u8 r) {
    if (r < 0x40) {
        if (r == 0x37) snes_ppu_latch_hv((int)((snes_master - line_start) / 4), cur_line);
        return snes_ppu_read(r);
    }
    if (r < 0x80) { apu_catch_up(snes_master); return snes_apu_read(r & 3); }
    if (r == 0x80) { u8 v = wram[wmadd]; wmadd = (wmadd + 1) & 0x1FFFF; return v; }
    return mdr;
}
static void bbus_write(u8 r, u8 v) {
    if (r < 0x40) { snes_ppu_write(r, v); return; }
    if (r < 0x80) { apu_catch_up(snes_master); snes_apu_write(r & 3, v); return; }
    switch (r) {
    case 0x80: wram[wmadd] = v; wmadd = (wmadd + 1) & 0x1FFFF; break;
    case 0x81: wmadd = (wmadd & 0x1FF00) | v; break;
    case 0x82: wmadd = (wmadd & 0x100FF) | (u32)v << 8; break;
    case 0x83: wmadd = (wmadd & 0x0FFFF) | (u32)(v & 1) << 16; break;
    default: break;
    }
}

/* ------------------------------------------------------- CPU registers */
static void update_hdma_nothing(void) {}

static void joy_auto_read(void) {
    for (int p = 0; p < 2; p++) {
        u16 b = pad_state[p], v = 0;
        /* B Y Select Start Up Down Left Right A X L R, msb first */
        if (b & PAD_A) v |= 0x8000;
        if (b & PAD_X) v |= 0x4000;
        if (b & PAD_MODE) v |= 0x2000;
        if (b & PAD_START) v |= 0x1000;
        if (b & PAD_UP) v |= 0x0800;
        if (b & PAD_DOWN) v |= 0x0400;
        if (b & PAD_LEFT) v |= 0x0200;
        if (b & PAD_RIGHT) v |= 0x0100;
        if (b & PAD_B) v |= 0x0080;
        if (b & PAD_Y) v |= 0x0040;
        if (b & PAD_C) v |= 0x0020;
        if (b & PAD_Z) v |= 0x0010;
        joy_auto[p] = v;
    }
}

static void dma_run(u8 mask);
static void memory_map(void);

static u8 reg_read(u32 off) {
    switch (off) {
    case 0x4016: case 0x4017: {
        int p = off & 1;
        u8 v = (u8)(joy_shift[p] >> 15);
        joy_shift[p] = (u16)(joy_shift[p] << 1 | 1);
        return (u8)((mdr & 0xFC) | v);
    }
    case 0x4210: { u8 v = (u8)((nmi_flag ? 0x80 : 0) | 0x02 | (mdr & 0x70)); nmi_flag = 0; return v; }
    case 0x4211: { u8 v = (u8)((irq_flag ? 0x80 : 0) | (mdr & 0x7F)); irq_flag = 0; w65.irq_line = 0; return v; }
    case 0x4212: {
        s64 h = snes_master - line_start;
        return (u8)((in_vblank ? 0x80 : 0) | ((h < 88 || h >= 1096) ? 0x40 : 0) | (joy_busy ? 1 : 0) | (mdr & 0x3E));
    }
    case 0x4213: return rdio;
    case 0x4214: return (u8)rddiv;
    case 0x4215: return (u8)(rddiv >> 8);
    case 0x4216: return (u8)rdmpy;
    case 0x4217: return (u8)(rdmpy >> 8);
    case 0x4218: return (u8)joy_auto[0];
    case 0x4219: return (u8)(joy_auto[0] >> 8);
    case 0x421A: return (u8)joy_auto[1];
    case 0x421B: return (u8)(joy_auto[1] >> 8);
    case 0x421C: case 0x421D: case 0x421E: case 0x421F: return 0;
    default: break;
    }
    if (off >= 0x4300 && off < 0x4380) {
        Chan *c = &ch[(off >> 4) & 7];
        switch (off & 0xF) {
        case 0: return c->dmap;  case 1: return c->bbad;
        case 2: return (u8)c->a1t; case 3: return (u8)(c->a1t >> 8); case 4: return c->a1b;
        case 5: return (u8)c->das; case 6: return (u8)(c->das >> 8); case 7: return c->dasb;
        case 8: return (u8)c->a2a; case 9: return (u8)(c->a2a >> 8); case 0xA: return c->ntrl;
        default: return c->unused;
        }
    }
    return mdr;
}

static void reg_write(u32 off, u8 v) {
    switch (off) {
    case 0x4016: if (joy_strobe && !(v & 1)) { joy_auto_read(); joy_shift[0] = joy_auto[0]; joy_shift[1] = joy_auto[1]; } joy_strobe = v & 1; return;
    case 0x4200:
        if ((v & 0x80) && !(nmitimen & 0x80) && nmi_flag) { w65.nmi_pending = 1; snes_slice_end = 0; }
        nmitimen = v;
        if (!(v & 0x30)) { irq_flag = 0; w65.irq_line = 0; }
        return;
    case 0x4201: rdio = v; return;
    case 0x4202: wrmpya = v; return;
    case 0x4203: rdmpy = (u16)(wrmpya * v); return;
    case 0x4204: wrdiv = (wrdiv & 0xFF00) | v; return;
    case 0x4205: wrdiv = (u16)((wrdiv & 0xFF) | v << 8); return;
    case 0x4206:
        if (v) { rddiv = wrdiv / v; rdmpy = wrdiv % v; } else { rddiv = 0xFFFF; rdmpy = wrdiv; }
        return;
    case 0x4207: htime = (htime & 0x100) | v; return;
    case 0x4208: htime = (u16)((htime & 0xFF) | (v & 1) << 8); return;
    case 0x4209: vtime = (vtime & 0x100) | v; return;
    case 0x420A: vtime = (u16)((vtime & 0xFF) | (v & 1) << 8); return;
    case 0x420B: dma_run(v); return;
    case 0x420C: hdmaen = v; return;
    case 0x420D: if ((memsel ^ v) & 1) { memsel = v; memory_map(); } memsel = v; snes_rom_speed = (v & 1) ? 6 : 8; return;
    default: break;
    }
    if (off >= 0x4300 && off < 0x4380) {
        Chan *c = &ch[(off >> 4) & 7];
        switch (off & 0xF) {
        case 0: c->dmap = v; break;  case 1: c->bbad = v; break;
        case 2: c->a1t = (c->a1t & 0xFF00) | v; break;
        case 3: c->a1t = (u16)((c->a1t & 0xFF) | v << 8); break;
        case 4: c->a1b = v; break;
        case 5: c->das = (c->das & 0xFF00) | v; break;
        case 6: c->das = (u16)((c->das & 0xFF) | v << 8); break;
        case 7: c->dasb = v; break;
        case 8: c->a2a = (c->a2a & 0xFF00) | v; break;
        case 9: c->a2a = (u16)((c->a2a & 0xFF) | v << 8); break;
        case 0xA: c->ntrl = v; break;
        default: c->unused = v; break;
        }
    }
}

/* -------------------------------------------------------- A bus access */
static u8 bus_peek(u32 a) {
    u32 bank = a >> 16, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) return wram[a & 0x1FFFF];
    if (!(bank & 0x40)) {
        if (off < 0x2000) return wram[off];
        if (off >= 0x2100 && off < 0x2200) return bbus_read((u8)off);
        if (off >= 0x4000 && off < 0x4400) return reg_read(off);
    }
    u8 v;
    if (cart_read(a, &v)) return v;
    return mdr;
}
static void bus_poke(u32 a, u8 v) {
    u32 bank = a >> 16, off = a & 0xFFFF;
    if (bank == 0x7E || bank == 0x7F) { wram[a & 0x1FFFF] = v; return; }
    if (!(bank & 0x40)) {
        if (off < 0x2000) { wram[off] = v; return; }
        if (off >= 0x2100 && off < 0x2200) { bbus_write((u8)off, v); return; }
        if (off >= 0x4000 && off < 0x4400) { reg_write(off, v); return; }
    }
    cart_write(a, v);
}
static inline int access_time(u32 a) {
    u32 bank = a >> 16, off = a & 0xFFFF;
    if (!(bank & 0x40)) {
        if (off < 0x2000 || (off >= 0x6000 && off < 0x8000)) return 8;
        if (off < 0x4000) return 6;
        if (off < 0x4200) return 12;
        if (off < 0x6000) return 6;
    }
    if (bank >= 0x7E && bank < 0x80) return 8;
    return (bank & 0x80) && (memsel & 1) ? 6 : 8;
}

#ifdef SNES_LOCKSTEP
/* Dev check, make smk-lockstep: every recompiled block runs, then the
 * interpreter replays the same instructions. The second run sees the first
 * one's bus trace instead of memory, so it must make the same accesses in the
 * same order and end with the same registers and the same cycle count. */
typedef struct { u32 a; u8 v, wr; s32 extra; } Trace;
static Trace trace[4096];
static int tr_n, tr_i, tr_mode, tr_bad, tr_err;
static u8 tr_read(u32 a) {
    if (tr_i < tr_n && !trace[tr_i].wr && trace[tr_i].a == a) { snes_master += access_time(a) + trace[tr_i].extra; return trace[tr_i++].v; }
    if (snes_rmap[a >> 12] && !snes_wmap[a >> 12]) { snes_master += access_time(a); return snes_rmap[a >> 12][a & 0xFFF]; }   /* opcode fetch */
    tr_bad = 1;
    snes_master += access_time(a);
    return 0;
}
static void tr_write(u32 a, u8 v) {
    if (tr_i < tr_n && trace[tr_i].wr && trace[tr_i].a == a && trace[tr_i].v == v) {
        snes_master += access_time(a) + trace[tr_i].extra; tr_i++;
        if (!(a & 0x400000) && (a & 0xFFFF) == 0x420D) memsel = v;   /* later fetches run at the new speed */
        return;
    }
    tr_bad = 1;
    snes_master += access_time(a);
}
#endif

u8 w65_bus_read(u32 a) {
#ifdef SNES_LOCKSTEP
    if (tr_mode == 2) return tr_read(a);
    if (tr_mode == 1) {
        s32 m = snes_master + access_time(a);
        u8 v = bus_peek(a); mdr = v; snes_master = m;
        s32 extra = 0;
        if (tr_n < 4096) trace[tr_n++] = (Trace){ a, v, 0, extra };
        return v;
    }
#endif
    const u8 *p = snes_rmap[a >> 12];
    if (p) { snes_master += snes_tmap[a >> 12]; return mdr = p[a & 0xFFF]; }
    snes_master += access_time(a);
    return mdr = bus_peek(a);
}
void w65_bus_write(u32 a, u8 v) {
#ifdef SNES_LOCKSTEP
    if (tr_mode == 2) { tr_write(a, v); return; }
    if (tr_mode == 1) {
        s32 m = snes_master;
        mdr = v; bus_poke(a, v);
        s32 extra = snes_master - m;           /* DMA */
        snes_master += access_time(a);
        if (tr_n < 4096) trace[tr_n++] = (Trace){ a, v, 1, extra };
        return;
    }
#endif
    u8 *p = snes_wmap[a >> 12];
    mdr = v;
    if (p) { snes_master += snes_tmap[a >> 12]; p[a & 0xFFF] = v; return; }
    snes_master += access_time(a);
    bus_poke(a, v);
}
void w65_bus_idle(void) { snes_master += 6; }

/* A page goes in the maps when all 4 KB of it are WRAM, or ROM that is
 * contiguous there. Everything else takes the slow path through bus_peek(). */
static void memory_map(void) {
    for (u32 pg = 0; pg < 4096; pg++) {
        u32 a = pg << 12, bank = a >> 16, off = a & 0xFFFF;
        const u8 *r = NULL; u8 *w = NULL;
        if (bank == 0x7E || bank == 0x7F) r = w = wram + (a & 0x1FFFF);
        else if (!(bank & 0x40) && off < 0x2000) r = w = wram + off;
        else {
            s32 i = rom_index(a), j = rom_index(a + 0xFFF);
            if (i >= 0 && j == i + 0xFFF && (u32)j < rom_size) r = rom + i;
        }
        snes_rmap[pg] = r; snes_wmap[pg] = w; snes_tmap[pg] = (u8)access_time(a);
    }
}

/* ---------------------------------------------------------- general DMA */
static void dma_run(u8 mask) {
    snes_master += 8;
    for (int i = 0; i < 8; i++) if (mask & (1 << i)) {
        Chan *c = &ch[i];
        int mode = c->dmap & 7, to_a = c->dmap & 0x80;
        int step = (c->dmap & 8) ? 0 : (c->dmap & 0x10) ? -1 : 1;
        u32 n = c->das ? c->das : 0x10000;
        snes_master += 8;
        for (u32 k = 0; k < n; k++) {
            u32 a = (u32)c->a1b << 16 | c->a1t;
            u8 b = (u8)(c->bbad + dma_pat[mode][k & 3]);
            if (to_a) bus_poke(a, bbus_read(b)); else bbus_write(b, bus_peek(a));
            c->a1t = (u16)(c->a1t + step);
            snes_master += 8;
        }
        c->das = 0;
    }
}

/* ----------------------------------------------------------------- HDMA */
static void hdma_load_ntrl(Chan *c) {
    c->ntrl = bus_peek((u32)c->a1b << 16 | c->a2a); c->a2a++;
    if (!c->ntrl) c->done = 1;
    if ((c->dmap & 0x40) && !c->done) {
        u8 lo = bus_peek((u32)c->a1b << 16 | c->a2a); c->a2a++;
        u8 hi = bus_peek((u32)c->a1b << 16 | c->a2a); c->a2a++;
        c->das = (u16)(lo | hi << 8);
        snes_master += 16;
    }
    c->do_transfer = 1;
    snes_master += 8;
}
static void hdma_init(void) {
    for (int i = 0; i < 8; i++) {
        Chan *c = &ch[i];
        c->done = 0; c->do_transfer = 0;
        if (hdmaen & (1 << i)) { c->a2a = c->a1t; hdma_load_ntrl(c); }
    }
}
static void hdma_line(void) {
    for (int i = 0; i < 8; i++) {
        Chan *c = &ch[i];
        if (!(hdmaen & (1 << i)) || c->done) continue;
        if (c->do_transfer) {
            int mode = c->dmap & 7, n = dma_len[mode], ind = c->dmap & 0x40, to_a = c->dmap & 0x80;
            for (int k = 0; k < n; k++) {
                u32 a;
                if (ind) { a = (u32)c->dasb << 16 | c->das; c->das++; }
                else     { a = (u32)c->a1b << 16 | c->a2a; c->a2a++; }
                u8 b = (u8)(c->bbad + dma_pat[mode][k]);
                if (to_a) bus_poke(a, bbus_read(b)); else bbus_write(b, bus_peek(a));
                snes_master += 8;
            }
        }
        c->ntrl = (u8)((c->ntrl & 0x80) | ((c->ntrl - 1) & 0x7F));
        c->do_transfer = c->ntrl & 0x80;
        if (!(c->ntrl & 0x7F)) hdma_load_ntrl(c);
    }
}

/* ------------------------------------------------------------ scheduler */
static void line_irq_setup(int line) {
    irq_at = -1;
    int mode = (nmitimen >> 4) & 3;
    if (!mode) return;
    if (mode == 2 || mode == 3) { if (line != vtime) return; }
    if (mode == 2) irq_at = line_start;                     /* V only: start of line */
    else irq_at = line_start + (s64)htime * 4;              /* H, or H and V */
}

/* -------------------------------------------- recompiled block dispatch
 * Blocks are keyed by 24-bit PC and the M/X flags, because the same bytes
 * decode differently in 8- and 16-bit modes. Anything else (emulation mode,
 * code in RAM, addresses the recompiler never saw) runs on the interpreter. */
#include <stdio.h>
#include <stdlib.h>
static char rom_title[24];
typedef void (*w65fn)(void);
static w65fn *block_bank[256];
int snes_force_interp;
void (*snes_register_game)(void);
SnesGameEntry snes_game_lib;
/* The recorder: addresses that fell to the interpreter, kept in memory and added
 * to a file every few seconds. The file stays closed in between, so File Sharing
 * can fetch it during play. */
static char miss_path[512], miss_buf[0x4000];
static int miss_len;
static u8 *miss_seen;
int snes_recording = 1;
char snes_record_path[512];
u32 snes_recorded;
void snes_record_flush(void) {
    if (!miss_len) return;
    FILE *f = fopen(miss_path, "a");
    if (f) { fwrite(miss_buf, 1, (size_t)miss_len, f); fclose(f); }
    miss_len = 0;
}
u64 snes_blocks_run, snes_interp_run;
u32 snes_native_cycles, snes_interp_cycles;
u64 snes_native_total, snes_interp_total;   /* the whole run, waiting left out */

void snes_recomp_add(const SnesBlock *t, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        u32 b = (t[i].pc >> 16) & 0xFF;
        if (!block_bank[b]) block_bank[b] = calloc(0x40000, sizeof(w65fn));
        block_bank[b][(t[i].pc & 0xFFFF) << 2 | (u32)t[i].mx] = t[i].f;
    }
}

#ifdef SNES_LOCKSTEP
static void run_block(w65fn f, u32 pc, int mx) {
    W65816 before = w65;
    s32 m0 = snes_master;
    u8 speed0 = memsel;
    tr_n = tr_i = 0; tr_mode = 1; tr_bad = 0;
    f();
    W65816 after = w65;
    s32 m1 = snes_master;
    w65 = before; snes_master = m0; memsel = speed0; tr_mode = 2;
    int guard = 0;
    do w65_exec();
    while (!(w65.pc == after.pc && w65.pb == after.pb && snes_master >= m1) && ++guard < 300);
    tr_mode = 0;
    int same = w65.a == after.a && w65.x == after.x && w65.y == after.y && w65.s == after.s && w65.d == after.d &&
               w65.db == after.db && w65.p == after.p && w65.waiting == after.waiting && w65.stopped == after.stopped;
    if (tr_bad || !same || tr_i != tr_n || snes_master != m1) {
        if (tr_err++ < 30)
            fprintf(stderr, "block %06X mx%d: bad %d regs %d trace %d/%d cycles %d/%d  A %04X/%04X X %04X/%04X Y %04X/%04X S %04X/%04X P %02X/%02X\n",
                    pc, mx, tr_bad, same, tr_i, tr_n, m1 - m0, snes_master - m0, after.a, w65.a, after.x, w65.x, after.y, w65.y, after.s, w65.s, after.p, w65.p);
    }
    w65 = after; snes_master = m1;
    { static u64 seen; if (++seen % 1000000 == 0) fprintf(stderr, "lockstep: %llu blocks, %d differ\n", (unsigned long long)seen, tr_err); }
}
#define CALL_BLOCK(f, pc, mx) run_block(f, pc, mx)
#else
#define CALL_BLOCK(f, pc, mx) f()
#endif

static inline void cpu_slice(void) {
    static u32 next_pc = 0xFFFFFFFF;
    if (!w65.e && !w65.waiting && !w65.stopped && !w65.nmi_pending && !(w65.irq_line && !(w65.p & 0x04))) {
        u32 pc = (u32)w65.pb << 16 | w65.pc;
        int mx = (w65.p >> 4) & 3;
        w65fn *t = snes_force_interp ? NULL : block_bank[w65.pb];
        w65fn f = t ? t[(pc & 0xFFFF) << 2 | (u32)mx] : NULL;
        if (f) {
            const s32 m = snes_master;
            CALL_BLOCK(f, pc, mx);
            snes_blocks_run++;
            snes_native_cycles += (u32)(snes_master - m);
            snes_native_total += (u32)(snes_master - m);
            next_pc = 0xFFFFFFFF;
            return;
        }
        if (miss_seen && snes_recording && pc != next_pc && ((w65.pc >= 0x8000 && w65.pb < 0x7E) || w65.pb >= 0x80 || (w65.pb >= 0x40 && w65.pb < 0x7E))) {
            u32 k = pc << 2 | (u32)mx;
            if (!(miss_seen[k >> 3] & (1 << (k & 7)))) {
                miss_seen[k >> 3] |= (u8)(1 << (k & 7));
                miss_len += snprintf(miss_buf + miss_len, 16, "%06X %d\n", pc, mx);
                snes_recorded++;
                if (miss_len > (int)sizeof miss_buf - 16) snes_record_flush();
            }
        }
        u32 before = pc;
        const s32 m = snes_master;
        w65_step();
        snes_interp_cycles += (u32)(snes_master - m);
        snes_interp_total += (u32)(snes_master - m);
        snes_interp_run++;
        u32 after = (u32)w65.pb << 16 | w65.pc;
        next_pc = (after > before && after - before <= 4) ? after : 0xFFFFFFFF;
        return;
    }
    const s32 m = snes_master;
    const int idle = w65.waiting || w65.stopped;
    w65_step();
    snes_interp_cycles += (u32)(snes_master - m);
    if (!idle) snes_interp_total += (u32)(snes_master - m);
    next_pc = 0xFFFFFFFF;
}

void snes_run_frame(void) {
    int vis = snes_ppu_overscan() ? 239 : 224;
    s32 base = snes_master;
    for (int line = 0; line < SNES_LINES_PER_FRAME; line++) {
        cur_line = line;
        line_start = base;
        s32 line_end = base + SNES_MASTER_PER_LINE;
        if (line == 0) {
            in_vblank = 0; nmi_flag = 0;
            snes_ppu_frame_start();
            hdma_init();
        } else if (line <= vis) {
            hdma_line();
            ppu_line(line);
        } else if (line == vis + 1) {
            in_vblank = 1; nmi_flag = 1;
            if (nmitimen & 0x80) w65.nmi_pending = 1;
            snes_ppu_vblank();
            if (nmitimen & 1) { joy_auto_read(); joy_busy = 1; }
        } else if (line == vis + 4) joy_busy = 0;
        line_irq_setup(line);
        while (snes_master < line_end) {
            if (irq_at >= 0 && snes_master >= irq_at) { irq_flag = 1; w65.irq_line = 1; irq_at = -1; }
            snes_slice_end = irq_at >= 0 && irq_at < line_end ? irq_at : line_end;
#ifdef SNES_LOCKSTEP
            snes_slice_end = 0;
#endif
            cpu_slice();
        }
        base = line_end;
        apu_catch_up(snes_master);
    }
    (void)update_hdma_nothing;
    { static int fr; if (++fr >= 300) { fr = 0; snes_record_flush(); } }
    /* only the distance between two counts matters, so start each frame near 0 */
    snes_master -= base;
    snes_apu_rebase(base);
}

/* ----------------------------------------------------------- start-up */
static int score_header(u32 base) {
    if (base + 32 > rom_size) return -1;
    const u8 *h = rom + base;
    int s = 0;
    u16 chk = (u16)(h[0x1E] | h[0x1F] << 8), inv = (u16)(h[0x1C] | h[0x1D] << 8);
    if ((u16)(chk ^ inv) == 0xFFFF) s += 8;
    for (int i = 0; i < 21; i++) if (h[i] >= 32 && h[i] < 127) s++;
    u8 map = h[0x15] & 0x0F;
    if ((base == 0xFFC0 && (map == 1 || map == 5)) || (base == 0x7FC0 && (map == 0 || map == 2 || map == 3))) s += 4;
    return s;
}

int snes_detect(void) {
    return score_header(0x7FC0) >= 16 || score_header(0xFFC0) >= 16;
}

u32 snes_rom_crc(void) {
    u32 c = 0xFFFFFFFF;
    for (u32 i = 0; i < rom_size; i++) {
        c ^= rom[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

const char *snes_load(void) {
    char *title = rom_title;
    if (rom_size % 1024 == 512) {                  /* copier header */
        rom_size -= 512;
        memmove(rom, rom + 512, rom_size);
    }
    hirom = score_header(0xFFC0) > score_header(0x7FC0);
    const u8 *h = rom + (hirom ? 0xFFC0 : 0x7FC0);
    memcpy(title, h, 21); title[21] = 0;
    for (int i = 20; i >= 0 && title[i] == ' '; i--) title[i] = 0;
    u8 type = h[0x16];
    has_dsp1 = (type & 0x0F) >= 3 && (type & 0xF0) == 0;     /* $03-$06: ROM + DSP */
    int ram = h[0x18];
    sram_mask = ram ? (u32)(0x400 << ram) - 1 : 0;
    if (sram_mask >= sizeof sram) sram_mask = sizeof sram - 1;
    return title;
}

void snes_reset(void) {
    static int registered;
    if (!registered) {
        registered = 1;
        if (snes_game_lib) {
            static const SnesEnv env = { &w65, &snes_master, &snes_slice_end, &snes_rom_speed, snes_rmap, snes_wmap, snes_tmap, &snes_mdr,
                                         w65_bus_read, w65_bus_write, w65_bus_idle, w65_exec, snes_recomp_add };
            snes_game_lib(&env);
        } else if (snes_register_game) snes_register_game();
#ifdef GENEXIA_OS9
        /* the iMac records every play: "<title>.miss" beside the application */
        char name[32];
        snprintf(name, sizeof name, "%.26s.miss", rom_title);
        for (char *c = name; *c; c++) if (*c == ':') *c = '_';
        const char *p = name;
#else
        const char *p = snes_record_path[0] ? snes_record_path : getenv("SF2_MISSLOG");
#endif
        if (p) { snprintf(miss_path, sizeof miss_path, "%s", p); miss_seen = calloc(1 << 23, 1); atexit(snes_record_flush); }
    }
    memset(wram, 0x55, sizeof wram);
    memset(ch, 0xFF, sizeof ch);
    wmadd = 0; nmitimen = 0; memsel = 0; hdmaen = 0; snes_rom_speed = 8;
    nmi_flag = irq_flag = in_vblank = joy_busy = 0;
    irq_at = -1;
    snes_ppu_reset();
    snes_apu_reset();
    snes_dsp1_reset();
    memory_map();
    w65_reset();
}

const u8 *snes_sram(u32 *size) { *size = sram_mask + 1; return sram; }

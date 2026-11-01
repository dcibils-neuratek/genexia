/*
 * bus.c - Mega Drive memory map: 68k I/O area (Z80 window, controllers,
 * Z80 bus arbitration, VDP/PSG ports) and the Z80's own address space.
 */
#include <string.h>
#include "m68k.h"
#include "bus.h"

u8  rom[0x400000];
EMU_STATE u8  ram[0x10000];
u32 rom_size;

EMU_STATE u8  zram[0x2000];
EMU_STATE static u32 zbank;            /* 9-bit bank register -> 68k A15..A23 */
EMU_STATE int z80_busreq, z80_reset_line = 1;
int game_is_sf2 = 1;
u8  region_version = 0x20;   /* Japan NTSC, no expansion unit */

/* ------------------------------------------------------- controllers */
u16 pad_state[2];            /* PAD_* bits, 1 = pressed */
EMU_STATE static u8 port_data[3], port_ctrl[3];
EMU_STATE static u8 th_phase[2];
EMU_STATE static s64 th_time[2];

static u8 pad_read(int p) {
    u16 b = pad_state[p];
    u8 th = (port_data[p] & port_ctrl[p] & 0x40) ? 1 : (port_ctrl[p] & 0x40) ? 0 : 1;
    /* six-button protocol phase times out after ~1.5ms */
    if (cpu.cyc - th_time[p] > 11500) th_phase[p] = 0;
    u8 ph = th_phase[p];
    u8 v;
    if (th) {
        if (ph == 6) {   /* C B M X Y Z */
            v = (u8)(((b & PAD_C) ? 0 : 0x20) | ((b & PAD_B) ? 0 : 0x10) | ((b & PAD_MODE) ? 0 : 0x08) |
                     ((b & PAD_X) ? 0 : 0x04) | ((b & PAD_Y) ? 0 : 0x02) | ((b & PAD_Z) ? 0 : 0x01));
        } else {
            v = (u8)(((b & PAD_C) ? 0 : 0x20) | ((b & PAD_B) ? 0 : 0x10) | ((b & PAD_RIGHT) ? 0 : 0x08) |
                     ((b & PAD_LEFT) ? 0 : 0x04) | ((b & PAD_DOWN) ? 0 : 0x02) | ((b & PAD_UP) ? 0 : 0x01));
        }
        v |= 0x40;
    } else {
        v = (u8)(((b & PAD_START) ? 0 : 0x20) | ((b & PAD_A) ? 0 : 0x10));
        if (ph == 5) { /* low nibble all 0 identifies 6-button pad */ }
        else if (ph == 7) v |= 0x0F;
        else v |= (u8)(((b & PAD_DOWN) ? 0 : 0x02) | ((b & PAD_UP) ? 0 : 0x01));
    }
    /* output pins read back what was written */
    return (u8)((v & ~port_ctrl[p]) | (port_data[p] & port_ctrl[p]) ) & 0x7F;
}

static void pad_write(int p, u8 v) {
    u8 old_th = (port_data[p] & port_ctrl[p] & 0x40) || !(port_ctrl[p] & 0x40);
    port_data[p] = v;
    u8 new_th = (port_data[p] & port_ctrl[p] & 0x40) || !(port_ctrl[p] & 0x40);
    if (old_th != new_th) {
        if (cpu.cyc - th_time[p] > 11500) th_phase[p] = 0;
        th_phase[p] = (u8)((th_phase[p] + 1) & 7);
        th_time[p] = cpu.cyc;
    }
}

/* ------------------------------------------------------------ 68k IO */
static u8 io_reg_read(u32 a) {
    switch ((a >> 1) & 0xF) {
    case 0: return region_version;
    case 1: return pad_read(0);
    case 2: return pad_read(1);
    case 3: return port_data[2];
    case 4: return port_ctrl[0];
    case 5: return port_ctrl[1];
    case 6: return port_ctrl[2];
    default: return 0;
    }
}
static void io_reg_write(u32 a, u8 v) {
    switch ((a >> 1) & 0xF) {
    case 1: pad_write(0, v); break;
    case 2: pad_write(1, v); break;
    case 3: port_data[2] = v; break;
    case 4: port_ctrl[0] = v; break;
    case 5: port_ctrl[1] = v; break;
    case 6: port_ctrl[2] = v; break;
    default: break;
    }
}

static int z80_bus_owned(void) { return z80_busreq || z80_reset_line; }

u8 io_read8(u32 a) {
    if (a >= 0xA00000 && a < 0xA10000) {
        if (!z80_bus_owned()) return 0xFF;
        return z80_mem_read((u16)(a & 0x7FFF));
    }
    if (a >= 0xA10000 && a < 0xA10020) return io_reg_read(a);
    if (a == 0xA11100) return (z80_busreq && !z80_reset_line) ? 0x80 : 0x81;  /* bit0: 0 = granted */
    if (a == 0xA11101) return 0;
    if (a >= 0xC00000 && a < 0xC00020) {
        u16 w = io_read16(a & ~1u);
        return (u8)((a & 1) ? w : w >> 8);
    }
    return 0xFF;
}

u16 io_read16(u32 a) {
    if (a >= 0xA00000 && a < 0xA10000) {
        u8 v = io_read8(a);
        return (u16)(v << 8 | v);
    }
    if (a >= 0xA10000 && a < 0xA10020) { u8 v = io_reg_read(a); return (u16)(v << 8 | v); }
    if (a == 0xA11100) return (z80_busreq && !z80_reset_line) ? 0x0000 : 0x0100;
    if (a >= 0xC00000 && a < 0xC00020) {
        switch (a & 0x1E) {
        case 0x00: case 0x02: return vdp_read_data();
        case 0x04: case 0x06: return vdp_read_ctrl();
        case 0x08: case 0x0A: case 0x0C: case 0x0E: return vdp_read_hv();
        default: return 0;
        }
    }
    return 0xFFFF;
}

static void z80_reset_write(int line) {
    if (line && !z80_reset_line) { z80_reset(); ym2612_reset(); }
    z80_reset_line = line;
}

void io_write8(u32 a, u8 v) {
    if (a >= 0xA00000 && a < 0xA10000) {
        if (z80_bus_owned()) z80_mem_write((u16)(a & 0x7FFF), v);
        return;
    }
    if (a >= 0xA10000 && a < 0xA10020) { io_reg_write(a, v); return; }
    if (a == 0xA11100) { z80_busreq = v & 1; return; }
    if (a == 0xA11200) { z80_reset_write(!(v & 1)); return; }
    if (a >= 0xC00000 && a < 0xC00020) {
        if ((a & 0x1F) == 0x11 || (a & 0x1F) == 0x13 || (a & 0x1F) == 0x15 || (a & 0x1F) == 0x17) {
            audio_sync_68k();
            psg_write(v);
            return;
        }
        io_write16(a & ~1u, (u16)(v << 8 | v));
        return;
    }
}

void io_write16(u32 a, u16 v) {
    if (a >= 0xA00000 && a < 0xA10000) { io_write8(a, (u8)(v >> 8)); return; }
    if (a >= 0xA10000 && a < 0xA10020) { io_reg_write(a, (u8)v); return; }
    if (a == 0xA11100) { z80_busreq = (v >> 8) & 1; return; }
    if (a == 0xA11200) { z80_reset_write(!((v >> 8) & 1)); return; }
    if (a >= 0xC00000 && a < 0xC00020) {
        switch (a & 0x1E) {
        case 0x00: case 0x02: vdp_write_data(v); break;
        case 0x04: case 0x06: vdp_write_ctrl(v); break;
        case 0x10: case 0x12: case 0x14: case 0x16: audio_sync_68k(); psg_write((u8)v); break;
        default: break;
        }
    }
}

u16 bus_read16_dma(u32 addr) {
    addr &= 0xFFFFFE;
    return (u16)rd16(addr);
}

/* ------------------------------------------------------------- Z80 bus */
u8 z80_mem_read(u16 a) {
    if (a < 0x4000) return zram[a & 0x1FFF];
    if (a < 0x6000) return ym2612_read_status();
    if (a >= 0x8000) {
        u32 addr = (zbank << 15) | (a & 0x7FFF);
        if (addr >= 0xE00000) return ram[addr & 0xFFFF];  /* not really allowed; harmless */
        return (u8)rd8(addr);
    }
    if (a >= 0x7F00 && a < 0x7F20) return 0xFF;
    return 0xFF;
}

void z80_mem_write(u16 a, u8 v) {
    if (a < 0x4000) { zram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { audio_sync_z80(); ym2612_write(a & 3, v); return; }
    if (a < 0x6100) { zbank = ((zbank >> 1) | ((u32)(v & 1) << 8)) & 0x1FF; return; }
    if (a == 0x7F11 || a == 0x7F13 || a == 0x7F15 || a == 0x7F17) { audio_sync_z80(); psg_write(v); return; }
    if (a >= 0x8000) {
        u32 addr = (zbank << 15) | (a & 0x7FFF);
        if (addr >= 0xE00000) ram[addr & 0xFFFF] = v;
        else if (addr >= 0xC00000) io_write8(addr, v);
    }
}

u8 z80_io_read(u16 port) { (void)port; return 0xFF; }
void z80_io_write(u16 port, u8 v) { (void)port; (void)v; }

void bus_reset(void) {
    memset(ram, 0, sizeof ram);
    memset(zram, 0, sizeof zram);
    zbank = 0;
    z80_busreq = 0;
    z80_reset_line = 1;
    memset(port_data, 0, sizeof port_data);
    memset(port_ctrl, 0, sizeof port_ctrl);
}

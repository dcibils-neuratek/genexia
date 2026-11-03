/*
 * ps1_spu.c - the SPU's registers and its 512 KB of sound RAM. No voices
 * play yet: this is what the BIOS and games need to see to carry on.
 */
#include "ps1.h"

static u8 spu_ram[0x80000];
static u16 regs[0x200];
static u32 xfer_addr;

u16 spu_read(u32 addr) {
    const u32 r = (addr - 0x1f801c00) >> 1;
    if (r >= 0x200) return 0;
    if (addr == 0x1f801dae) return regs[0xd5] & 0x3f;     /* SPUSTAT follows SPUCNT, never busy */
    if (addr == 0x1f801da6) return (u16)(xfer_addr >> 3);
    return regs[r];
}

void spu_write(u32 addr, u16 v) {
    const u32 r = (addr - 0x1f801c00) >> 1;
    if (r >= 0x200) return;
    regs[r] = v;
    if (addr == 0x1f801da6) xfer_addr = (u32)v << 3;
    else if (addr == 0x1f801da8) { spu_ram[xfer_addr & 0x7ffff] = v; spu_ram[(xfer_addr + 1) & 0x7ffff] = v >> 8; xfer_addr += 2; }
    else if (addr == 0x1f801d88) regs[0xce] &= ~v;      /* key on clears ENDX */
}

void spu_dma_write(u32 w) {
    for (int i = 0; i < 4; i++) spu_ram[(xfer_addr + i) & 0x7ffff] = w >> (8 * i);
    xfer_addr = (xfer_addr + 4) & 0x7ffff;
}

u32 spu_dma_read(void) {
    u32 w = 0;
    for (int i = 0; i < 4; i++) w |= (u32)spu_ram[(xfer_addr + i) & 0x7ffff] << (8 * i);
    xfer_addr = (xfer_addr + 4) & 0x7ffff;
    return w;
}

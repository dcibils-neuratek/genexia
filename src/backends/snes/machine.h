#ifndef SNES_MACHINE_H
#define SNES_MACHINE_H
#include "genesis.h"
int  snes_detect(void);          /* rom[] holds a SNES cartridge */
const char *snes_load(void);     /* set up mapping from the header, returns the title */
void snes_reset(void);
void snes_run_frame(void);
/* Master clock within the frame: 32 bits, because every instruction adds to it
 * and a 64-bit add takes six PowerPC instructions. */
extern s32 snes_master;
extern s32 snes_slice_end;       /* recompiled blocks chain until this count */
extern int snes_rom_speed;
/* 4 KB pages of the 24-bit bus that are plain memory, NULL otherwise. */
extern const u8 *snes_rmap[4096];
extern u8 *snes_wmap[4096];
extern u8 snes_tmap[4096];       /* master cycles per access */
extern u8 snes_mdr;
extern int snes_force_interp;
extern u64 snes_blocks_run, snes_interp_run;
extern u32 snes_native_cycles, snes_interp_cycles;   /* CPU time by where it ran */
extern void (*snes_register_game)(void);   /* recompiled code of the loaded game */
typedef struct { unsigned pc; int mx; void (*f)(void); } SnesBlock;
void snes_recomp_add(const SnesBlock *b, unsigned n);
void recomp_register_smk(void);
#ifdef GENEXIA_OS9
extern u32 snes_t_ppu, snes_t_apu;   /* time spent there, for the log */
#endif
u8   w65_bus_read(u32 addr);
void w65_bus_write(u32 addr, u8 v);
static inline __attribute__((always_inline)) u8 snes_rd(u32 a) {
    const u8 *p = snes_rmap[a >> 12];
    if (!p) return w65_bus_read(a);
    snes_master += snes_tmap[a >> 12];
    return snes_mdr = p[a & 0xFFF];
}
static inline __attribute__((always_inline)) void snes_wr(u32 a, u8 v) {
    u8 *p = snes_wmap[a >> 12];
    if (!p) { w65_bus_write(a, v); return; }
    snes_master += snes_tmap[a >> 12];
    snes_mdr = v;
    p[a & 0xFFF] = v;
}
#endif

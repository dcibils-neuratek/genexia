#ifndef SNES_MACHINE_H
#define SNES_MACHINE_H
#include "genesis.h"
int  snes_detect(void);          /* rom[] holds a SNES cartridge */
const char *snes_load(void);     /* set up mapping from the header, returns the title */
void snes_reset(void);
void snes_run_frame(void);
extern s64 snes_master;
extern int snes_rom_speed;
extern int snes_force_interp;
extern u64 snes_blocks_run, snes_interp_run;
extern void (*snes_register_game)(void);   /* recompiled code of the loaded game */
void snes_recomp_add(u32 pc, int mx, void (*f)(void));
void recomp_register_smk(void);
#endif

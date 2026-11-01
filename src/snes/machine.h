#ifndef SNES_MACHINE_H
#define SNES_MACHINE_H
#include "../genesis.h"
int  snes_detect(void);          /* rom[] holds a SNES cartridge */
const char *snes_load(void);     /* set up mapping from the header, returns the title */
void snes_reset(void);
void snes_run_frame(void);
extern s64 snes_master;
#endif

#ifndef BUS_H
#define BUS_H
#include "genesis.h"

enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_A = 1 << 4, PAD_B = 1 << 5, PAD_C = 1 << 6, PAD_START = 1 << 7,
    PAD_X = 1 << 8, PAD_Y = 1 << 9, PAD_Z = 1 << 10, PAD_MODE = 1 << 11,
};

extern u16 pad_state[2];
extern int z80_busreq, z80_reset_line;
extern u8  region_version;
extern int game_is_sf2;      /* the recompiled code and SF2 enhancements apply */
extern u8  zram[0x2000];

void bus_reset(void);
/* bring the sound chips up to "now" before a register write (main.c) */
void audio_sync_68k(void);
void audio_sync_z80(void);

#endif

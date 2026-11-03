/*
 * ps1.h - the PlayStation: an R3000A with its GTE, the GPU, the CD-ROM drive,
 * the SPU, timers, DMA and the pads. Written from the psx-spx documentation.
 *
 * Time is counted in CPU cycles (33.8688 MHz). Each part that wants to act
 * later asks for an event at a cycle; the CPU runs until the nearest one.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define PS1_CLOCK 33868800
#define LINE_CYCLES 2153            /* NTSC: 3413 video clocks a line */
#define FRAME_LINES 263
#define VISIBLE_LINES 240

extern u8 ps1_ram[0x200000], ps1_bios[0x80000], ps1_scratch[0x400];
extern u64 ps1_now, ps1_next;       /* the cycle it is, and the nearest event */

/* cpu */
typedef struct {
    u32 r[32], hi, lo;
    u32 pc, next_pc, cur_pc;
    u32 sr, cause, epc, badv;
    int branch, delay;
    u32 ld_r, ld_v;                 /* a load landing after the next instruction */
} Ps1Cpu;
extern Ps1Cpu cpu;
void cpu_reset(void);
void cpu_run(void);                 /* until ps1_now reaches ps1_next */
extern void (*cpu_hook)(void);      /* called when pc reaches cpu_hook_pc */
extern u32 cpu_hook_pc;

/* gte */
u32 gte_read(int reg);
void gte_write(int reg, u32 v);
void gte_command(u32 op);

/* bus */
u32 bus_read(u32 addr, int size);
void bus_write(u32 addr, u32 v, int size);
void irq_raise(int n);
void irq_update(void);

/* events */
enum { EV_LINE, EV_TIMER, EV_CD, EV_CD_SECTOR, EV_PAD, EV_COUNT };
void event_at(int id, u64 at);
void event_off(int id);
void events_run(void);
void timers_line(int line, int vblank);
void timers_event(void);

/* gpu */
extern u16 vram[1024 * 512];
void gpu_reset(void);
void gpu_gp0(u32 v);
void gpu_gp1(u32 v);
u32 gpu_read(void);
u32 gpu_stat(void);
void gpu_line(int line);
int gpu_frame(u32 *out, int *w, int *h);    /* the shown picture, as RGBA */
extern int gpu_dot_div;

/* cdrom */
int cd_open(const char *cue_or_bin);
void cd_reset(void);
u8 cd_read(int reg);
void cd_write(int reg, u8 v);
u32 cd_dma_word(void);
void cd_event(void);
void cd_sector_event(void);
int cd_read_sector(u32 lba, u8 *out2352);

/* spu */
u16 spu_read(u32 addr);
void spu_write(u32 addr, u16 v);
void spu_dma_write(u32 w);
u32 spu_dma_read(void);

/* mdec */
void mdec_write(int reg, u32 v);
u32 mdec_read(int reg);
void mdec_dma_in(u32 w);
u32 mdec_dma_out(void);
int mdec_has_output(void);
void dma_mdec_ready(void);      /* output waiting: a DMA asked for it may run now */

/* pads */
extern u16 pad_buttons;             /* PlayStation bits, 1 = pressed */
u32 sio_read(int reg, int size);
void sio_write(int reg, u32 v, int size);
void pad_event(void);

/*
 * genesis.h - shared contract between all runtime components of the
 * Street Fighter II' (Mega Drive) static recompilation.
 *
 * Every chip module (vdp.c, z80.c, ym2612.c, psg.c) only depends on this
 * header and on the callbacks declared in it; bus.c / main.c provide them.
 *
 * Clocks (NTSC): master 53693175 Hz, 68000 = master/7 = 7670453 Hz,
 * Z80 = master/15 = 3579545 Hz, 262 lines/frame, ~488 68k cycles/line,
 * ~228 Z80 cycles/line. YM2612 native sample rate = 68k clock / 144 =
 * 53267 Hz (one sample = 6 * 24 FM-clock operator slots).
 */
#ifndef GENESIS_H
#define GENESIS_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;  typedef int8_t  s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

/* Every variable that is part of the emulated machine's state is tagged
 * EMU_STATE: the linker gathers them into one section, so a save state is a
 * single memcpy of that section (state.c). Host-side data stays outside. */
#ifdef __MACH__
#define EMU_STATE __attribute__((section("__DATA,__emustate")))
#else
#define EMU_STATE                  /* the classic Mac build has no save states */
#endif

/* biggest cartridge: 64 MB for the N64; the classic Mac build is Genesis only */
#ifndef ROM_MAX
#ifdef GENEXIA_OS9
#define ROM_MAX 0x800000
#else
#define ROM_MAX 0x4000000
#endif
#endif

#define M68K_CLOCK      7670453
#define Z80_CLOCK       3579545
#define LINES_PER_FRAME 262
#define M68K_CYCLES_PER_LINE 488
#define Z80_CYCLES_PER_LINE  228
#define YM_SAMPLE_RATE  53267

/* ---------------------------------------------------------------- VDP */
void vdp_init(void);
void vdp_write_data(u16 v);
u16  vdp_read_data(void);
void vdp_write_ctrl(u16 v);
u16  vdp_read_ctrl(void);      /* status register; clears write-pending latch */
u16  vdp_read_hv(void);        /* HV counter, uses vdp's current line + sys_line_cycle() */
/* Called by the main loop at the START of every scanline 0..261, before the
 * CPUs run that line. Renders visible lines into vdp_framebuffer, maintains
 * the H-int counter, sets the vblank status flag at line == active height
 * (224 or 240) and raises the V-int pending flag there; clears vblank at 0. */
void vdp_begin_line(int line);
extern int vdp_skip_render;    /* 1 = frame will not be shown: keep timing/IRQs, skip drawing */
int  vdp_irq_pending(void);
extern int vdp_irq_level;      /* same value as vdp_irq_pending(), kept current by the VDP */    /* 6 if V-int pending&enabled, else 4 if H-int pending&enabled, else 0 */
void vdp_irq_ack(int level);   /* CPU took interrupt of that level -> clear its pending flag */
int  vdp_zint_pending(void);   /* 1 during the line V-blank begins (Z80 /INT) */
#define VDP_FB_STRIDE 512
extern u32 vdp_framebuffer[VDP_FB_STRIDE * 240];  /* 0x00RRGGBB */
extern int vdp_screen_w, vdp_screen_h;  /* output size incl. widescreen margins */
/* Widescreen: when enabled the VDP renders native_width*4/3 pixels per line,
 * extending planes and sprites into stage graphics that exist beyond the
 * game's view (TV-correct 16:9). Margins without stage data become black. */
extern int vdp_widescreen;
#ifdef GENEXIA_OS9
extern int vdp_altivec;                 /* the machine has it: see vdp_altivec.c */
extern unsigned vdp_altivec_lines;
void vdp_compose_altivec(u8 *out, const u8 *A, const u8 *B, const u8 *S, int n);
void vdp_compose_shadow_altivec(u8 *out, u8 *bank, const u8 *A, const u8 *B, const u8 *S, int n);
#endif
extern int vdp_native_w;                /* 256 or 320: the game's own view width */
extern int vdp_view_x;                  /* where game x=0 lands in the output line */
extern int vdp_no_sprite_limit;
extern int vdp_hud_rules;               /* which game's HUD sprites stay centred in widescreen: 0 none, 1 SF2, 2 UMK3 */
/* per output pixel: 1 = P1 fighter sprite (palette 2) won, 2 = P2 (palette 3) */
extern u8 vdp_fx_mask[240 * VDP_FB_STRIDE];         /* 1 = ignore the per-line sprite limits (no flicker) */

/* provided by bus.c for the VDP */
u16 bus_read16_dma(u32 addr);  /* 68k-side word read for 68k->VDP DMA */
int sys_line_cycle(void);      /* 68k cycles elapsed in the current line, 0..487 */

/* ---------------------------------------------------------------- Z80 */
void z80_reset(void);
int  z80_execute(int cycles);  /* runs at least `cycles` T-states, returns T-states actually run */
void z80_set_irq(int state);   /* level-triggered /INT line (IM1 is what games use) */
void z80_nmi(void);
/* provided by bus.c */
u8   z80_mem_read(u16 addr);
void z80_mem_write(u16 addr, u8 v);
u8   z80_io_read(u16 port);
void z80_io_write(u16 port, u8 v);

/* ------------------------------------------------------------- YM2612 */
void ym2612_reset(void);
void ym2612_write(int port, u8 v); /* port 0: addr A0, 1: data A0, 2: addr A1, 3: data A1 */
u8   ym2612_read_status(void);    /* bit7 busy, bit1 timer B overflow, bit0 timer A overflow */
/* Generate n stereo samples (interleaved L,R) at YM_SAMPLE_RATE; also advances timers. */
void ym2612_render(s16 *out, int n);
void ym2612_advance(int n);         /* timers only, no synthesis */

/* ---------------------------------------------------------------- PSG */
void psg_reset(void);
void psg_write(u8 v);
/* Generate n mono samples at YM_SAMPLE_RATE (psg clock = Z80 clock). */
void psg_render(s16 *out, int n);

#endif

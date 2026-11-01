/*
 * snes.h - contract between the SNES backend's components.
 *
 * The Genexia front end (src/main.c) is console-independent: it drives a
 * backend one frame at a time, takes pictures from vdp_framebuffer and audio
 * from a sample buffer. All emulated state is tagged EMU_STATE, so whole-machine
 * save states and run-ahead work without per-chip code.
 *
 * Clocks (NTSC): master 21477272 Hz, 1364 master cycles per scanline, 262
 * lines per frame. S-SMP 1024000 Hz, S-DSP output 32000 Hz.
 */
#ifndef SNES_H
#define SNES_H
#include "genesis.h"          /* u8..s64, EMU_STATE, vdp_framebuffer, vdp_skip_render */

#define SNES_MASTER_PER_LINE 1364
#define SNES_LINES_PER_FRAME 262
#define SNES_DSP_RATE 32000

/* ---------------------------------------------------------------- PPU (ppu.c)
 * Registers are addressed by their low byte: 0x00..0x3F for $2100..$213F. */
void snes_ppu_reset(void);
void snes_ppu_write(u8 reg, u8 v);     /* $2100-$2133 */
u8   snes_ppu_read(u8 reg);            /* $2134-$213F */
/* Called at the START of each scanline 1..224 (or ..239 when overscan is set)
 * with the screen line number; renders into vdp_framebuffer, stride
 * VDP_FB_STRIDE (512), 0x00RRGGBB, unless vdp_skip_render is set. Must set
 * vdp_screen_w/vdp_screen_h (256 or 512 / 224 or 239) and vdp_native_w. */
void snes_ppu_render_line(int line);
/* line/dot latch for $2137 and $213C-$213F; provided by bus.c */
void snes_ppu_latch_hv(int h, int v);
int  snes_ppu_overscan(void);          /* 1 = 240-line mode (SETINI bit 2) */
int  snes_ppu_forced_blank(void);      /* INIDISP bit 7 */
/* start of vblank: reload OAM address as hardware does */
void snes_ppu_vblank(void);
/* start of a frame (line 0): toggle interlace field etc. */
void snes_ppu_frame_start(void);

/* --------------------------------------------------------------- APU (apu.c)
 * SPC700 + 64 KB APU RAM + S-DSP + timers + IPL boot code. */
void snes_apu_reset(void);
u8   snes_apu_read(int port);          /* CPU side of $2140-$2143 (port 0-3) */
void snes_apu_write(int port, u8 v);
/* Run the SPC700 and DSP so they catch up to the master clock count `master`
 * (21.477 MHz cycles). Samples accumulate internally. The machine restarts the
 * count every frame and reports how far back it went. */
void snes_apu_run_to(s32 master);
void snes_apu_rebase(s32 by);
/* Drain up to `max` stereo frames (interleaved L,R) at SNES_DSP_RATE; returns frames copied. */
int  snes_apu_take(s16 *out, int max);

/* ------------------------------------------------------------- DSP-1 (dsp1.c)
 * High-level implementation of the DSP-1/1B math coprocessor (Super Mario
 * Kart, Pilotwings). 8-bit data register + status register. */
void snes_dsp1_reset(void);
u8   snes_dsp1_read(int is_status);
void snes_dsp1_write(u8 v);
#endif

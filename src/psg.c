/*
 * psg.c - SN76489 (Sega VDP variant) programmable sound generator.
 *
 * 3 square-wave tone channels + 1 noise channel, clocked at Z80 clock / 16.
 * Sega variant: 16-bit LFSR, white noise taps bits 0 and 3, periodic noise
 * feeds bit 0 back; tone periods of 0/1 output a constant level (used for
 * PCM-through-volume tricks). Output is box-filtered (exact area average of
 * the PSG tick stream) down to YM_SAMPLE_RATE.
 */
#include "genesis.h"
#include <math.h>
#include <string.h>

#define PSG_CH_PEAK 2560          /* peak amplitude per channel at volume 0 */
#define FRAC_ONE    65536u

static struct {
    u16 tone[3];
    u8 vol[4];
    u8 noise;
    u8 latch;                     /* 0..7: channel*2 + (1 = volume) */
    u16 cnt[4];
    u8 out[4];
    u8 nflip;
    u16 lfsr;
    u32 frac;                     /* fixed-point fraction left in current tick */
} psg EMU_STATE;

static s16 voltab[16];
static u32 step_fp;               /* PSG ticks per output sample, 16.16 */
static int ready;

static void init(void)
{
    for (int i = 0; i < 15; i++)
        voltab[i] = (s16)lround(PSG_CH_PEAK * pow(10.0, -0.1 * i));  /* 2 dB steps */
    voltab[15] = 0;
    step_fp = (u32)lround((double)Z80_CLOCK / 16.0 / YM_SAMPLE_RATE * FRAC_ONE);
    ready = 1;
}

void psg_reset(void)
{
    if (!ready) init();
    memset(&psg, 0, sizeof psg);
    for (int i = 0; i < 4; i++) { psg.vol[i] = 15; psg.out[i] = 1; }
    psg.lfsr = 0x8000;
    psg.frac = FRAC_ONE;
}

void psg_write(u8 v)
{
    if (!ready) psg_reset();
    int ch;
    if (v & 0x80) {
        psg.latch = (v >> 4) & 7;
        ch = psg.latch >> 1;
        if (psg.latch & 1) psg.vol[ch] = v & 15;
        else if (ch < 3) psg.tone[ch] = (u16)((psg.tone[ch] & 0x3f0) | (v & 15));
        else { psg.noise = v & 7; psg.lfsr = 0x8000; }
    } else {
        ch = psg.latch >> 1;
        if (psg.latch & 1) psg.vol[ch] = v & 15;
        else if (ch < 3) psg.tone[ch] = (u16)((psg.tone[ch] & 15) | ((v & 0x3f) << 4));
        else { psg.noise = v & 7; psg.lfsr = 0x8000; }
    }
}

static inline void tick(void)
{
    for (int c = 0; c < 3; c++) {
        if (psg.cnt[c]) psg.cnt[c]--;
        if (!psg.cnt[c]) {
            psg.cnt[c] = psg.tone[c];
            if (psg.tone[c] > 1) psg.out[c] ^= 1;
            else psg.out[c] = 1;
        }
    }
    if (psg.cnt[3]) psg.cnt[3]--;
    if (!psg.cnt[3]) {
        u16 p = (psg.noise & 3) == 3 ? psg.tone[2] : (u16)(0x10 << (psg.noise & 3));
        psg.cnt[3] = p ? p : 1;
        psg.nflip ^= 1;
        if (psg.nflip) {
            u16 fb = (psg.noise & 4) ? ((psg.lfsr ^ (psg.lfsr >> 3)) & 1) : (psg.lfsr & 1);
            psg.lfsr = (u16)((psg.lfsr >> 1) | (fb << 15));
            psg.out[3] = psg.lfsr & 1;
        }
    }
}

static inline int level(void)
{
    int s = 0;
    for (int c = 0; c < 4; c++) {
        int a = voltab[psg.vol[c]];
        s += psg.out[c] ? a : -a;
    }
    return s;
}

void psg_render(s16 *out, int n)
{
    if (!ready) psg_reset();
    for (int i = 0; i < n; i++) {
        s64 acc = 0;
        u32 rem = step_fp;
        while (rem) {
            u32 take = rem < psg.frac ? rem : psg.frac;
            acc += (s64)level() * take;
            rem -= take;
            psg.frac -= take;
            if (!psg.frac) { tick(); psg.frac = FRAC_ONE; }
        }
        out[i] = (s16)(acc / (s64)step_fp);
    }
}

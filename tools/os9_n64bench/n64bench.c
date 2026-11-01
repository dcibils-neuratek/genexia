/*
 * n64bench.c - runs Mario 64's compact recompiled code with no RSP and no
 * drawing, pressing Start through the title, and writes how long the CPU part
 * of each 100 video frames took to n64bench.log. The real console shows 60
 * video frames a second and the game moves on every second one.
 */
#include <stdio.h>
#include <stdlib.h>
#include "n64_machine.h"
#include "m68k.h"
#include "bus.h"
#ifdef GENEXIA_OS9
#include <Multiverse.h>
pascal void Microseconds(void *t);
static double now_ms(void) { unsigned long t[2]; Microseconds(t); return (t[0] * 4294967296.0 + t[1]) / 1000.0; }
#else
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
long long r4300_native_cycles, r4300_interp_cycles;
#endif
extern long long r4300_native_cycles, r4300_interp_cycles;

/* what r4300.c and machine.c want from the rest of the program */
u8 rom[ROM_MAX]; u32 rom_size;
u16 pad_state[2];
void rsp_reset(void) {}
int  rsp_run(int c) { return c; }
void rsp_set_pc(u32 pc) {}
u32  rsp_get_pc(void) { return 0; }
void rdp_reset(void) {}
void rdp_run(u32 s, u32 e, int x) {}
void rdp_wide_frame(int d) {}
void rdp_set_hires(int n, int w) {}
void rdp_hires_resync(void) {}
int  rdp_hires_fetch(u32 o, int a, u32 *p, int w, int h) { return 0; }

void recomp_register_sm64(void);
#define FRAMES 4000

extern u8 n64_rdram[];
static unsigned ram_sum(void) {
    unsigned h = 0;
    for (int i = 0; i < 0x800000; i += 4)
        h = (h << 1 | h >> 31) ^ ((unsigned)n64_rdram[i] << 24 | n64_rdram[i + 1] << 16 | n64_rdram[i + 2] << 8 | n64_rdram[i + 3]);
    return h;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "sm64.z64", "rb");
    FILE *log = fopen("n64bench.log", "w");
    freopen("n64bench.err", "w", stderr);
    if (!log) return 1;
    if (!f) { fprintf(log, "no sm64.z64\n"); fclose(log); return 1; }
    fseek(f, 0, SEEK_END); rom_size = (u32)ftell(f); fseek(f, 0, SEEK_SET);
    fprintf(log, "rom %u, read %u\n", rom_size, (u32)fread(rom, 1, rom_size, f));
    fflush(log); fclose(f);
    n64_load();
    n64_register_game = recomp_register_sm64;
    n64_reset();
    double t0 = now_ms(), worst = 0, last = t0;
    long long n0 = 0, i0 = 0;
    for (int fr = 1; fr <= FRAMES; fr++) {
        /* the same presses that reach the first course on the host */
        pad_state[0] = (fr >= 400 && fr < 410) || (fr >= 620 && fr < 630) || (fr >= 900 && fr < 910) || (fr >= 1200 && fr < 1210) ? PAD_START : 0;
        n64_run_frame();
        double t = now_ms();
        if (t - last > worst) worst = t - last;
        last = t;
        if (fr % 100 == 0) {
            fprintf(log, "frame %4d: %6.1f ms per video frame, worst %6.1f, native %ld M interp %ld K cycles, ram %08X\n",
                    fr, (t - t0) / 100, worst, (long)((r4300_native_cycles - n0) / 1000000), (long)((r4300_interp_cycles - i0) / 1000), ram_sum());
            fflush(log);
            n0 = r4300_native_cycles; i0 = r4300_interp_cycles; t0 = t; worst = 0;
        }
    }
    n64_debug_print();
    fprintf(log, "done\n");
    fclose(log);
    return 0;
}

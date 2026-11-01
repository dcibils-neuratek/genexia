/*
 * m68k_core.c - exception processing, reset and the block dispatcher that
 * runs statically recompiled code, falling back to the interpreter for any
 * address the recompiler did not discover (logged for the next recompile).
 */
#include <stdio.h>
#include <stdlib.h>
#include "m68k.h"

EMU_STATE M68K cpu;
int m68k_force_interp;

void (*m68k_register_game)(void) = recomp_register;   /* recompiled code of the loaded game */
static blockfn *blocktab;           /* indexed by pc>>1 for pc < 0x400000 */
static u8 *miss_seen;               /* ROM addresses already logged as misses */
static FILE *miss_log;
u64 m68k_blocks_run, m68k_interp_run;

void recomp_add(u32 pc, blockfn f) {
    if (pc < 0x400000) blocktab[pc >> 1] = f;
}

void m68k_exception(int vec, u32 ret) {
    u32 sr = get_sr();
    if (!cpu.s) { cpu.usp = cpu.a[7]; cpu.a[7] = cpu.ssp; cpu.s = 1; }
    cpu.t = 0;
    push32(ret);
    push16(sr);
    cpu.pc = rd32((u32)vec * 4);
    cpu.stopped = 0;
}

void m68k_reset(void) {
    if (!blocktab) {
        blocktab = calloc(0x200000, sizeof(blockfn));
        miss_seen = calloc(0x200000, 1);
        if (m68k_register_game) m68k_register_game();
        const char *p = getenv("SF2_MISSLOG");
        if (p) miss_log = fopen(p, "a");
    }
    cpu.s = 1; cpu.t = 0; cpu.imask = 7; cpu.stopped = 0;
    cpu.a[7] = cpu.ssp = rd32(0);
    cpu.pc = rd32(4);
}

void m68k_interp_step(void) {
    cpu.ppc = cpu.pc;
    cpu.ir = fetch16();
    m68k_optable[cpu.ir]();
}

static void log_miss(u32 pc) {
    if (pc >= 0x400000 || miss_seen[pc >> 1]) return;
    miss_seen[pc >> 1] = 1;
    if (miss_log) { fprintf(miss_log, "%06X\n", pc); fflush(miss_log); }
}

void m68k_run(s64 until) {
    static u32 interp_next = 0xFFFFFFFF;   /* pc that sequential interpretation would reach; survives time slices */
    while (cpu.cyc < until) {
        int lvl = vdp_irq_level;
        if (lvl > cpu.imask || lvl == 7) {
            m68k_exception(24 + lvl, cpu.pc);
            cpu.imask = (u8)lvl;
            vdp_irq_ack(lvl);
            cpu.cyc += 44;
            interp_next = 0xFFFFFFFF;
        }
        if (cpu.stopped) { cpu.cyc = until; return; }
        u32 pc = cpu.pc & 0xFFFFFF;
        blockfn f = (pc < 0x400000 && !m68k_force_interp) ? blocktab[pc >> 1] : NULL;
        if (f) {
            f();
            m68k_blocks_run++;
            interp_next = 0xFFFFFFFF;
        } else {
            if (pc != interp_next) log_miss(pc);
            m68k_interp_step();
            m68k_interp_run++;
            interp_next = cpu.ppc + 2;  /* approximate: only used to detect jumps */
            if (cpu.pc > cpu.ppc && cpu.pc - cpu.ppc <= 10) interp_next = cpu.pc;
        }
    }
}

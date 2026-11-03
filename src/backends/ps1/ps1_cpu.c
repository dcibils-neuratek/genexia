/*
 * ps1_cpu.c - the R3000A interpreter: load and branch delay slots, the
 * exceptions of coprocessor 0, and the way into the GTE.
 *
 * A load lands one instruction late: the instruction after it still reads the
 * old value, unless it writes that register itself first.
 */
#include "ps1.h"

Ps1Cpu cpu;
void (*cpu_hook)(void);
u32 cpu_hook_pc = 0xffffffff;
int cpu_tty;

#define OP_RS ((op >> 21) & 31)
#define OP_RT ((op >> 16) & 31)
#define OP_RD ((op >> 11) & 31)
#define OP_SH ((op >> 6) & 31)
#define IMM ((u32)(s32)(s16)op)
#define UIMM (op & 0xffff)

enum { EX_INT = 0, EX_ADEL = 4, EX_ADES = 5, EX_SYS = 8, EX_BP = 9, EX_RI = 10, EX_CPU = 11, EX_OV = 12 };

static u32 pend_r, pend_v;          /* the load landing after this instruction */

void cpu_reset(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0xbfc00000;
    cpu.next_pc = cpu.pc + 4;
    cpu.sr = 0x00400000;            /* BEV: exceptions go to the BIOS */
}

static inline void W(int i, u32 v) {
    cpu.r[i] = v;
    if ((u32)i == pend_r) pend_r = 0;
}

static inline void L(int i, u32 v) { cpu.ld_r = i; cpu.ld_v = v; }

static void exception(int code) {
    cpu.cause = (cpu.cause & ~0xb000007cu) | (code << 2);
    u32 epc = cpu.cur_pc;
    if (cpu.delay) { epc -= 4; cpu.cause |= 1u << 31; }
    if (code == EX_CPU) cpu.cause |= 2u << 28;      /* only the FPU and COP3 are missing */
    cpu.epc = epc;
    cpu.sr = (cpu.sr & ~0x3fu) | ((cpu.sr << 2) & 0x3f);
    cpu.pc = cpu.sr & (1 << 22) ? 0xbfc00180 : 0x80000080;
    cpu.next_pc = cpu.pc + 4;
    cpu.branch = 0;
}

static inline u32 fetch(u32 pc) {
    const u32 a = pc & 0x1fffffff;
    u32 v;
    if (a < 0x800000) { memcpy(&v, ps1_ram + (a & 0x1ffffc), 4); return v; }
    if (a >= 0x1fc00000 && a < 0x1fc80000) { memcpy(&v, ps1_bios + (a & 0x7fffc), 4); return v; }
    return bus_read(pc, 4);
}

static inline u32 load(u32 a, int size) {
    const u32 p = a & 0x1fffffff;
    if (p < 0x800000) {
        const u8 *m = ps1_ram + (p & 0x1fffff);
        return size == 4 ? (u32)m[0] | m[1] << 8 | m[2] << 16 | (u32)m[3] << 24 : size == 2 ? (u32)(m[0] | m[1] << 8) : m[0];
    }
    if (p >= 0x1f800000 && p < 0x1f800400) {
        const u8 *m = ps1_scratch + (p & 0x3ff);
        return size == 4 ? (u32)m[0] | m[1] << 8 | m[2] << 16 | (u32)m[3] << 24 : size == 2 ? (u32)(m[0] | m[1] << 8) : m[0];
    }
    return bus_read(a, size);
}

static inline void store(u32 a, u32 v, int size) {
    if (cpu.sr & 0x10000) return;   /* the cache is isolated: the BIOS is clearing it */
    const u32 p = a & 0x1fffffff;
    u8 *m = p < 0x800000 ? ps1_ram + (p & 0x1fffff) : p >= 0x1f800000 && p < 0x1f800400 ? ps1_scratch + (p & 0x3ff) : NULL;
    if (!m) { bus_write(a, v, size); return; }
    m[0] = v;
    if (size >= 2) m[1] = v >> 8;
    if (size == 4) { m[2] = v >> 16; m[3] = v >> 24; }
}

static inline void jump(u32 to) { cpu.next_pc = to; cpu.branch = 1; }
static inline void branch_if(int c, u32 op) { if (c) jump(cpu.pc + (IMM << 2)); else cpu.branch = 1; }

static void cop0(u32 op) {
    switch (OP_RS) {
    case 0: {       /* mfc0 */
        const int r = OP_RD;
        L(OP_RT, r == 12 ? cpu.sr : r == 13 ? cpu.cause : r == 14 ? cpu.epc : r == 8 ? cpu.badv : r == 15 ? 2 : 0);
        break;
    }
    case 4: {       /* mtc0 */
        const u32 v = cpu.r[OP_RT];
        if (OP_RD == 12) cpu.sr = v;
        else if (OP_RD == 13) cpu.cause = (cpu.cause & ~0x300u) | (v & 0x300);
        break;
    }
    case 16:        /* rfe */
        cpu.sr = (cpu.sr & ~0xfu) | ((cpu.sr >> 2) & 0xf);
        break;
    default: exception(EX_RI);
    }
}

static void cop2(u32 op) {
    if (!(cpu.sr & (1u << 30))) { exception(EX_CPU); cpu.cause = (cpu.cause & ~(3u << 28)) | 2u << 28; return; }
    if (op & (1 << 25)) { gte_command(op); return; }
    switch (OP_RS) {
    case 0: L(OP_RT, gte_read(OP_RD)); break;
    case 2: L(OP_RT, gte_read(OP_RD + 32)); break;
    case 4: gte_write(OP_RD, cpu.r[OP_RT]); break;
    case 6: gte_write(OP_RD + 32, cpu.r[OP_RT]); break;
    default: exception(EX_RI);
    }
}

static void special(u32 op) {
    const u32 s = cpu.r[OP_RS], t = cpu.r[OP_RT];
    switch (op & 63) {
    case 0x00: W(OP_RD, t << OP_SH); break;
    case 0x02: W(OP_RD, t >> OP_SH); break;
    case 0x03: W(OP_RD, (u32)((s32)t >> OP_SH)); break;
    case 0x04: W(OP_RD, t << (s & 31)); break;
    case 0x06: W(OP_RD, t >> (s & 31)); break;
    case 0x07: W(OP_RD, (u32)((s32)t >> (s & 31))); break;
    case 0x08: jump(s); break;
    case 0x09: { const u32 back = cpu.next_pc; jump(s); W(OP_RD, back); break; }
    case 0x0c: exception(EX_SYS); break;
    case 0x0d: exception(EX_BP); break;
    case 0x10: W(OP_RD, cpu.hi); break;
    case 0x11: cpu.hi = s; break;
    case 0x12: W(OP_RD, cpu.lo); break;
    case 0x13: cpu.lo = s; break;
    case 0x18: { const s64 m = (s64)(s32)s * (s32)t; cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); break; }
    case 0x19: { const u64 m = (u64)s * t; cpu.lo = (u32)m; cpu.hi = (u32)(m >> 32); break; }
    case 0x1a:
        if (t == 0) { cpu.hi = s; cpu.lo = (s32)s >= 0 ? 0xffffffff : 1; }
        else if (s == 0x80000000 && t == 0xffffffff) { cpu.hi = 0; cpu.lo = s; }
        else { cpu.lo = (u32)((s32)s / (s32)t); cpu.hi = (u32)((s32)s % (s32)t); }
        break;
    case 0x1b:
        if (t == 0) { cpu.hi = s; cpu.lo = 0xffffffff; }
        else { cpu.lo = s / t; cpu.hi = s % t; }
        break;
    case 0x20: { const u32 r = s + t; if (~(s ^ t) & (s ^ r) & 0x80000000) exception(EX_OV); else W(OP_RD, r); break; }
    case 0x21: W(OP_RD, s + t); break;
    case 0x22: { const u32 r = s - t; if ((s ^ t) & (s ^ r) & 0x80000000) exception(EX_OV); else W(OP_RD, r); break; }
    case 0x23: W(OP_RD, s - t); break;
    case 0x24: W(OP_RD, s & t); break;
    case 0x25: W(OP_RD, s | t); break;
    case 0x26: W(OP_RD, s ^ t); break;
    case 0x27: W(OP_RD, ~(s | t)); break;
    case 0x2a: W(OP_RD, (s32)s < (s32)t); break;
    case 0x2b: W(OP_RD, s < t); break;
    default: exception(EX_RI);
    }
}

static void execute(u32 op) {
    const u32 s = cpu.r[OP_RS];
    switch (op >> 26) {
    case 0x00: special(op); break;
    case 0x01: {
        const int rt = OP_RT, ge = rt & 1, link = (rt & 0x1e) == 0x10;
        const int c = ge ? (s32)s >= 0 : (s32)s < 0;
        if (link) W(31, cpu.next_pc);
        branch_if(c, op);
        break;
    }
    case 0x02: jump((cpu.pc & 0xf0000000) | ((op & 0x3ffffff) << 2)); break;
    case 0x03: W(31, cpu.next_pc); jump((cpu.pc & 0xf0000000) | ((op & 0x3ffffff) << 2)); break;
    case 0x04: branch_if(s == cpu.r[OP_RT], op); break;
    case 0x05: branch_if(s != cpu.r[OP_RT], op); break;
    case 0x06: branch_if((s32)s <= 0, op); break;
    case 0x07: branch_if((s32)s > 0, op); break;
    case 0x08: { const u32 r = s + IMM; if (~(s ^ IMM) & (s ^ r) & 0x80000000) exception(EX_OV); else W(OP_RT, r); break; }
    case 0x09: W(OP_RT, s + IMM); break;
    case 0x0a: W(OP_RT, (s32)s < (s32)IMM); break;
    case 0x0b: W(OP_RT, s < IMM); break;
    case 0x0c: W(OP_RT, s & UIMM); break;
    case 0x0d: W(OP_RT, s | UIMM); break;
    case 0x0e: W(OP_RT, s ^ UIMM); break;
    case 0x0f: W(OP_RT, UIMM << 16); break;
    case 0x10: cop0(op); break;
    case 0x12: cop2(op); break;
    case 0x11: case 0x13: exception(EX_CPU); cpu.cause = (cpu.cause & ~(3u << 28)) | (u32)((op >> 26) & 3) << 28; break;
    case 0x20: L(OP_RT, (u32)(s32)(s8)load(s + IMM, 1)); break;
    case 0x21: { const u32 a = s + IMM; if (a & 1) { cpu.badv = a; exception(EX_ADEL); } else L(OP_RT, (u32)(s32)(s16)load(a, 2)); break; }
    case 0x22: case 0x26: {         /* lwl, lwr: they see a load still in flight to the same register */
        const u32 a = s + IMM, w = load(a & ~3u, 4), sh = (a & 3) * 8;
        const u32 old = (u32)OP_RT == pend_r ? pend_v : cpu.r[OP_RT];
        const u32 v = (op >> 26) == 0x22 ? (old & (0x00ffffffu >> sh)) | (w << (24 - sh))
                                           : (old & ~(0xffffffffu >> sh)) | (w >> sh);
        L(OP_RT, v);
        break;
    }
    case 0x23: { const u32 a = s + IMM; if (a & 3) { cpu.badv = a; exception(EX_ADEL); } else L(OP_RT, load(a, 4)); break; }
    case 0x24: L(OP_RT, load(s + IMM, 1)); break;
    case 0x25: { const u32 a = s + IMM; if (a & 1) { cpu.badv = a; exception(EX_ADEL); } else L(OP_RT, load(a, 2)); break; }
    case 0x28: store(s + IMM, cpu.r[OP_RT], 1); break;
    case 0x29: { const u32 a = s + IMM; if (a & 1) { cpu.badv = a; exception(EX_ADES); } else store(a, cpu.r[OP_RT], 2); break; }
    case 0x2a: case 0x2e: {
        const u32 a = s + IMM, w = load(a & ~3u, 4), sh = (a & 3) * 8, t = cpu.r[OP_RT];
        const u32 v = (op >> 26) == 0x2a ? (w & ~(0xffffffffu >> (24 - sh))) | (t >> (24 - sh))
                                           : (w & ~(0xffffffffu << sh)) | (t << sh);
        store(a & ~3u, v, 4);
        break;
    }
    case 0x2b: { const u32 a = s + IMM; if (a & 3) { cpu.badv = a; exception(EX_ADES); } else store(a, cpu.r[OP_RT], 4); break; }
    case 0x32: { const u32 a = s + IMM; gte_write(OP_RT, load(a, 4)); break; }
    case 0x3a: { const u32 a = s + IMM; store(a, gte_read(OP_RT), 4); break; }
    case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3b: exception(EX_CPU); break;
    default: exception(EX_RI);
    }
}

void cpu_run(void) {
    while (ps1_now < ps1_next) {
        if (cpu.pc == cpu_hook_pc && cpu_hook) cpu_hook();
        if (cpu_tty) {      /* the BIOS's putchar */
            const u32 p = cpu.pc & 0x1fffffff;
            if ((p == 0xa0 && cpu.r[9] == 0x3c) || (p == 0xb0 && cpu.r[9] == 0x3d)) fputc((int)cpu.r[4], stderr);
        }
        if ((cpu.sr & 1) && (cpu.sr & cpu.cause & 0xff00)) {
            const u32 next = fetch(cpu.pc);
            if ((next & 0xfe000000) == 0x4a000000) gte_command(next);   /* the BIOS skips it on return */
            cpu.cur_pc = cpu.pc;
            cpu.delay = cpu.branch;
            pend_r = cpu.ld_r; pend_v = cpu.ld_v;
            cpu.ld_r = 0;
            exception(EX_INT);
            if (pend_r) cpu.r[pend_r] = pend_v;
            cpu.r[0] = 0;
            continue;
        }
        cpu.cur_pc = cpu.pc;
        if (cpu.pc & 3) { cpu.badv = cpu.pc; cpu.delay = cpu.branch; exception(EX_ADEL); continue; }
        const u32 op = fetch(cpu.pc);
        cpu.delay = cpu.branch;
        cpu.branch = 0;
        cpu.pc = cpu.next_pc;
        cpu.next_pc += 4;
        pend_r = cpu.ld_r; pend_v = cpu.ld_v;
        cpu.ld_r = 0;
        execute(op);
        if (pend_r) cpu.r[pend_r] = pend_v;
        cpu.r[0] = 0;
        ps1_now += 2;
    }
}

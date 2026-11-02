/* Code the game put in memory itself, which no recompile could see: it loads
 * pieces of its sound engine for each race. One basic block at a time, the same
 * meaning as tools/recompile_psp.py gives each instruction. Included by psp_runtime.c. */
static u64 interpreted;

static int interp_special(u32 inst) {
    const u32 rs = inst >> 21 & 31, rt = inst >> 16 & 31, rd = inst >> 11 & 31, sa = inst >> 6 & 31, fn = inst & 63;
    const u32 a = p.r[rs], b = p.r[rt];
    u32 v;
    switch (fn) {
    case 0: v = b << sa; break;
    case 2: if (rs == 1) v = psp_ror(b, sa); else if (rs == 0) v = b >> sa; else return 0; break;
    case 3: v = (u32)((s32)b >> sa); break;
    case 4: v = b << (a & 31); break;
    case 6: v = sa == 1 ? psp_ror(b, a) : b >> (a & 31); break;
    case 7: v = (u32)((s32)b >> (a & 31)); break;
    case 10: if (b) return 1; v = a; break;
    case 11: if (!b) return 1; v = a; break;
    case 15: return 1;
    case 16: v = p.hi; break;
    case 17: p.hi = a; return 1;
    case 18: v = p.lo; break;
    case 19: p.lo = a; return 1;
    case 22: v = psp_clz(a); break;
    case 23: v = psp_clz(~a); break;
    case 24: { const s64 m = (s64)(s32)a * (s32)b; p.lo = (u32)m; p.hi = (u32)(m >> 32); return 1; }
    case 25: { const u64 m = (u64)a * b; p.lo = (u32)m; p.hi = (u32)(m >> 32); return 1; }
    case 26: psp_div((s32)a, (s32)b); return 1;
    case 27: psp_divu(a, b); return 1;
    case 28: psp_acc((s64)(s32)a * (s32)b); return 1;
    case 29: psp_acc((s64)((u64)a * b)); return 1;
    case 46: psp_acc(-((s64)(s32)a * (s32)b)); return 1;
    case 47: psp_acc(-(s64)((u64)a * b)); return 1;
    case 32: case 33: v = a + b; break;
    case 34: case 35: v = a - b; break;
    case 36: v = a & b; break;
    case 37: v = a | b; break;
    case 38: v = a ^ b; break;
    case 39: v = ~(a | b); break;
    case 42: v = (s32)a < (s32)b; break;
    case 43: v = a < b; break;
    case 44: v = (s32)a > (s32)b ? a : b; break;
    case 45: v = (s32)a < (s32)b ? a : b; break;
    default: return 0;
    }
    p.r[rd] = v;
    return 1;
}

static int interp_cop1(u32 inst) {
    const u32 rs = inst >> 21 & 31, rt = inst >> 16 & 31, fs = inst >> 11 & 31, fd = inst >> 6 & 31, fn = inst & 63;
    const float a = p.f[fs].s, b = p.f[rt].s;
    switch (rs) {
    case 0: p.r[rt] = p.f[fs].w; return 1;
    case 2: p.r[rt] = fs == 31 ? p.fcr31 : fs == 0 ? 0x3351u : 0; return 1;
    case 4: p.f[fs].w = p.r[rt]; return 1;
    case 6: if (fs == 31) p.fcr31 = p.r[rt]; return 1;
    case 20: if (fn != 32) return 0; p.f[fd].s = (float)p.f[fs].i; return 1;
    case 16: break;
    default: return 0;
    }
    switch (fn) {
    case 0: p.f[fd].s = a + b; return 1;
    case 1: p.f[fd].s = a - b; return 1;
    case 2: p.f[fd].s = a * b; return 1;
    case 3: p.f[fd].s = a / b; return 1;
    case 4: p.f[fd].s = sqrtf(a); return 1;
    case 5: p.f[fd].s = fabsf(a); return 1;
    case 6: p.f[fd].s = a; return 1;
    case 7: p.f[fd].s = -a; return 1;
    case 12: p.f[fd].i = psp_f2i(roundf(rintf(a))); return 1;
    case 13: p.f[fd].i = psp_f2i(truncf(a)); return 1;
    case 14: p.f[fd].i = psp_f2i(ceilf(a)); return 1;
    case 15: p.f[fd].i = psp_f2i(floorf(a)); return 1;
    case 36: p.f[fd].i = psp_f2i(rintf(a)); return 1;
    }
    if (fn < 48) return 0;
    const int c = (fn & 1 && (a != a || b != b)) || (fn & 2 && a == b) || (fn & 4 && a < b);
    p.fcr31 = (p.fcr31 & ~(1u << 23)) | (u32)c << 23;
    return 1;
}

/* one instruction that isn't a branch; 0 when it isn't an instruction */
static int interp_alu(u32 pc, u32 inst) {
    const u32 op = inst >> 26, rs = inst >> 21 & 31, rt = inst >> 16 & 31, imm = inst & 0xFFFF;
    const s32 simm = (s16)imm;
    const u32 a = p.r[rs], ea = a + (u32)simm;
    switch (op) {
    case 0:
        if ((inst & 63) == 12) return 0;
        if ((inst & 63) == 13) { psp_trap(pc, inst); return 0; }
        return interp_special(inst);
    case 8: case 9: p.r[rt] = a + (u32)simm; return 1;
    case 10: p.r[rt] = (s32)a < simm; return 1;
    case 11: p.r[rt] = a < (u32)simm; return 1;
    case 12: p.r[rt] = a & imm; return 1;
    case 13: p.r[rt] = a | imm; return 1;
    case 14: p.r[rt] = a ^ imm; return 1;
    case 15: p.r[rt] = imm << 16; return 1;
    case 17: return interp_cop1(inst);
    case 31: {
        const u32 rd = inst >> 11 & 31, sa = inst >> 6 & 31, fn = inst & 63, b = p.r[rt];
        if (fn == 0) { p.r[rt] = (a >> sa) & (u32)((1ull << (rd + 1)) - 1); return 1; }
        if (fn == 4) { const u32 m = (u32)(((1ull << (rd - sa + 1)) - 1) << sa); p.r[rt] = (b & ~m) | ((a << sa) & m); return 1; }
        if (fn != 32) return 0;
        switch (sa) {
        case 16: p.r[rd] = (u32)(s32)(s8)b; return 1;
        case 24: p.r[rd] = (u32)(s32)(s16)b; return 1;
        case 20: p.r[rd] = psp_bitrev(b); return 1;
        case 2: p.r[rd] = psp_ror(__builtin_bswap32(b), 16); return 1;
        case 3: p.r[rd] = __builtin_bswap32(b); return 1;
        }
        return 0;
    }
    case 32: p.r[rt] = (u32)(s32)(s8)RD8(ea); return 1;
    case 33: p.r[rt] = (u32)(s32)(s16)RD16(ea); return 1;
    case 34: p.r[rt] = psp_lwl(p.r[rt], ea); return 1;
    case 35: case 48: p.r[rt] = RD32(ea); return 1;
    case 36: p.r[rt] = RD8(ea); return 1;
    case 37: p.r[rt] = RD16(ea); return 1;
    case 38: p.r[rt] = psp_lwr(p.r[rt], ea); return 1;
    case 40: WR8(ea, (u8)p.r[rt]); return 1;
    case 41: WR16(ea, (u16)p.r[rt]); return 1;
    case 42: psp_swl(p.r[rt], ea); return 1;
    case 43: WR32(ea, p.r[rt]); return 1;
    case 46: psp_swr(p.r[rt], ea); return 1;
    case 47: return 1;
    case 49: p.f[rt].w = RD32(ea); return 1;
    case 56: WR32(ea, p.r[rt]); p.r[rt] = 1; return 1;
    case 57: WR32(ea, p.f[rt].w); return 1;
    case 50: case 53: case 54: case 58: case 61: case 62: psp_vfpu_load(inst); return 1;
    case 18: case 24: case 25: case 27: case 52: case 55: case 60: psp_vfpu(inst); return 1;
    case 63: return inst >> 16 == 0xFFFF;
    }
    return 0;
}

/* 0 not a branch, 1 a jump to a register or address, 2 conditional, 3 conditional likely */
static int interp_branch(u32 pc, u32 inst, int *taken, u32 *target, int *link) {
    const u32 op = inst >> 26, rs = inst >> 21 & 31, rt = inst >> 16 & 31;
    const u32 a = p.r[rs], b = p.r[rt], t = pc + 4 + (u32)((s32)(s16)inst << 2);
    *link = -1; *taken = 1; *target = t;
    if (op == 0 && (inst & 63) == 8) { *target = a; return 1; }
    if (op == 0 && (inst & 63) == 9) { *target = a; *link = inst >> 11 & 31; return 1; }
    if (op == 1 && (rt & ~0x13u) == 0) {
        *taken = rt & 1 ? (s32)a >= 0 : (s32)a < 0;
        if (rt & 16) *link = 31;
        return rt & 2 ? 3 : 2;
    }
    if (op == 2 || op == 3) { *target = ((pc + 4) & 0xF0000000) | (inst & 0x03FFFFFF) << 2; if (op == 3) *link = 31; return 1; }
    const u32 base = op >= 20 && op <= 23 ? op - 16 : op;
    if (base >= 4 && base <= 7) {
        *taken = base == 4 ? a == b : base == 5 ? a != b : base == 6 ? (s32)a <= 0 : (s32)a > 0;
        return op >= 20 ? 3 : 2;
    }
    if (op == 17 && rs == 8) { *taken = (p.fcr31 >> 23 & 1) == (rt & 1); return rt & 2 ? 3 : 2; }
    if (op == 18 && rs == 8) { *taken = (p.vcc >> (inst >> 18 & 7) & 1) == (rt & 1); return rt & 2 ? 3 : 2; }
    return 0;
}

static u32 psp_interp(u32 pc) {
    static char t[64];
    interpreted++;
    for (const u32 first = pc;; pc += 4) {
        const u32 slot = (pc - CODE_BASE) >> 2;
        if (slot >= CODE_SLOTS || (pc != first && (import_of[slot] || blocks[slot]))) return pc;
        const u32 inst = RD32(pc);
        int taken, link;
        u32 target;
        const int kind = interp_branch(pc, inst, &taken, &target, &link);
        if (kind) {
            const u32 next = RD32(pc + 4);
            int a, c; u32 d;
            if (interp_branch(pc + 4, next, &a, &d, &c)) { snprintf(t, sizeof t, "a branch in a delay slot at %08X", pc); stop_reason = t; return pc; }
            if (link > 0) p.r[link] = pc + 8;
            if (kind == 3 && !taken) return pc + 8;
            if (!interp_alu(pc + 4, next) && !stop_reason) { snprintf(t, sizeof t, "not an instruction at %08X: %08X", pc + 4, next); stop_reason = t; }
            p.r[0] = 0;
            return taken ? target : pc + 8;
        }
        if (!interp_alu(pc, inst)) {
            if (!stop_reason) { snprintf(t, sizeof t, "not an instruction at %08X: %08X", pc, inst); stop_reason = t; }
            return pc;
        }
        p.r[0] = 0;
    }
}

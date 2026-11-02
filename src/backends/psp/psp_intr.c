/* Game code the system runs by itself: vblank handlers, alarms, GE callbacks.
 * Included by psp_runtime.c. These only fire between blocks, at a system call
 * or while the scheduler waits, and run to their end on a stack of their own.
 * A handler can't block: waits inside one return at once. */
#define INTR_RETURN 0x08000010u
#define INTR_STACK  0x08803FF0u
#define VBLANK_US   16683

static int in_intr;
static struct { u32 fn, arg; int on; } vblank_sub[32];
static struct { u32 fn, arg; u64 at; } alarms[16];
static u32 ge_signal_fn, ge_signal_arg, ge_finish_fn, ge_finish_arg;
static u64 next_vblank_at = VBLANK_US;
int psp_ge_finishes, psp_ge_signals;

static void hle(int i);

static u32 call_guest(u32 pc, u32 a0, u32 a1) {
    const PspCpu saved = p;
    p.pc = pc; p.r[4] = a0; p.r[5] = a1; p.r[29] = INTR_STACK; p.r[31] = INTR_RETURN;
    in_intr = 1;
    while (p.pc != INTR_RETURN && !stop_reason) {
        const u32 slot = (p.pc - CODE_BASE) >> 2;
        if (slot >= CODE_SLOTS) { stop_reason = "a handler jumped outside the program"; break; }
        if (import_of[slot]) { p.pc = p.r[31]; hle(import_of[slot] - 1); continue; }
        p.pc = blocks[slot] ? blocks[slot]() : psp_interp(p.pc);
        p.r[0] = 0;
    }
    in_intr = 0;
    const u32 result = p.r[2];
    p = saved;
    return result;
}

static int vblank_wanted(void) {
    for (int i = 0; i < 32; i++) if (vblank_sub[i].on && vblank_sub[i].fn) return 1;
    return 0;
}

/* when the next one is due, so a scheduler with nothing to run can wait for it */
static u64 intr_next(void) {
    u64 soon = vblank_wanted() ? next_vblank_at : ~0ull;
    for (int i = 0; i < 16; i++) if (alarms[i].fn && alarms[i].at < soon) soon = alarms[i].at;
    return soon;
}

static void psp_tick(void) {
    if (in_intr) return;
    if (now_us >= next_vblank_at) {
        next_vblank_at = (now_us / VBLANK_US + 1) * VBLANK_US;
        for (int i = 0; i < 32; i++) if (vblank_sub[i].on && vblank_sub[i].fn) call_guest(vblank_sub[i].fn, (u32)i, vblank_sub[i].arg);
    }
    for (int i = 0; i < 16; i++) if (alarms[i].fn && alarms[i].at <= now_us) {
        const u32 again = call_guest(alarms[i].fn, alarms[i].arg, 0);    /* a nonzero answer sets it again */
        if (again) alarms[i].at = now_us + again; else alarms[i].fn = 0;
    }
    for (; psp_ge_signals > 0; psp_ge_signals--) if (ge_signal_fn) call_guest(ge_signal_fn, 0, ge_signal_arg);
    for (; psp_ge_finishes > 0; psp_ge_finishes--) if (ge_finish_fn) call_guest(ge_finish_fn, 0, ge_finish_arg);
}

static u32 set_alarm(u64 us, u32 fn, u32 arg) {
    for (int i = 0; i < 16; i++) if (!alarms[i].fn) {
        alarms[i].fn = fn; alarms[i].arg = arg; alarms[i].at = now_us + us;
        return 0x5000u + (u32)i;
    }
    return 0x800201A9u;
}

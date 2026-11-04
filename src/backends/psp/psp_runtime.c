/*
 * psp_runtime.c - runs a recompiled PSP game: its memory, threads, files and the
 * system functions it imports. For now this is the boot tool: it logs what
 * the game asks of the system and says where it stopped.
 *
 *   psp_boot GAME.elf GAME.iso [seconds]
 *
 * Threads switch only inside system calls, so one register store is enough. Time
 * is virtual: it follows the code run, and jumps ahead when every thread waits.
 */
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include "psp_os.h"
#include <time.h>
#include "allegrex.h"
#include "psp_nids.h"

PspCpu p;
u8 *psp_mem;
void psp_register_game(void);
extern u64 psp_vfpu_unknown;
extern u32 psp_vfpu_missing[64];
extern int psp_vfpu_nmissing;
extern u64 psp_ge_tris, psp_ge_prims, psp_ge_unknown[256];
u32 psp_ge_enqueue(u32 list, u32 stall);
void psp_ge_update_stall(u32 id, u32 stall);
void psp_ge_new_frame(void);
void psp_ge_shot(const char *path, u32 addr, int stride, int fmt);

#define CODE_BASE 0x08800000u
#define CODE_SLOTS (0x01800000u / 4)
#define IDLE_PC   0x08000000u              /* a thread returning from its entry lands here */
static PspBlock *blocks;
static u16 *import_of;
static struct { const char *module; u32 nid; const char *name; u32 calls; } imports[1024];
static int nimports;
static u64 now_us, ran;
static FILE *lg;
static const char *stop_reason;

void psp_block_add(u32 pc, PspBlock fn) { blocks[(pc - CODE_BASE) >> 2] = fn; }
void psp_import_add(u32 pc, const char *module, u32 nid) {
    imports[nimports].module = module;
    imports[nimports].nid = nid;
    for (unsigned i = 0; i < sizeof psp_names / sizeof *psp_names; i++)
        if (psp_names[i].nid == nid) imports[nimports].name = psp_names[i].name;
    import_of[(pc - CODE_BASE) >> 2] = ++nimports;
}
u32 psp_trap(u32 pc, u32 inst) { static char t[64]; snprintf(t, sizeof t, "trap at %08X: %08X", pc, inst); stop_reason = t; return 0; }

/* --------------------------------------------------------------- threads */
enum { FREE, READY, WAIT_TIME, WAIT_SEMA, WAIT_FLAG, WAIT_SLEEP, WAIT_END, DONE };
typedef struct {
    int state, prio, id, need, mode, wakeups, intr_off; u32 out, stack, stack_size; u64 until; int timed; char name[32];
    PspCpu cpu;
} Thread;
#define MAX_THREADS 64
static Thread th[MAX_THREADS];
static int cur;
static int intr_off;      /* the running thread is inside sceKernelCpuSuspendIntr: nothing may cut in */
#define UID(i) (0x1000 + (i))
#define TID(u) ((int)(u) - 0x1000)
#define ERR_NOT_FOUND 0x80020198u

static u32 stack_top = 0x09FFFF00u, heap = 0;
static u32 alloc_low(u32 n) { u32 a = heap; heap = (heap + n + 63) & ~63u; return a; }
static u32 alloc_high(u32 n) { stack_top = (stack_top - n) & ~63u; return stack_top; }

/* a deleted thread's stack waits here for the next thread of its size */
static struct { u32 at, size; } spare_stacks[MAX_THREADS];
static u32 stack_for(u32 size) {
    for (int i = 0; i < MAX_THREADS; i++) if (spare_stacks[i].size == size) { spare_stacks[i].size = 0; return spare_stacks[i].at; }
    return alloc_high(size);
}
static void delete_thread(Thread *t) {
    if (t->state == FREE) return;
    t->state = FREE;
    for (int i = 0; i < MAX_THREADS; i++) if (!spare_stacks[i].size) { spare_stacks[i].at = t->stack; spare_stacks[i].size = t->stack_size; return; }
}

static int new_thread(const char *name, u32 entry, int prio, u32 stack) {
    for (int i = 0; i < MAX_THREADS; i++) if (th[i].state == FREE) {
        Thread *t = &th[i];
        memset(t, 0, sizeof *t);
        snprintf(t->name, sizeof t->name, "%s", name);
        t->state = DONE;                    /* dormant until started */
        t->prio = prio;
        t->cpu.pc = entry;
        t->stack_size = stack ? stack : 0x40000;
        t->stack = stack_for(t->stack_size);
        const u32 top = t->stack + t->stack_size;
        t->cpu.r[29] = (top - 0x100) & ~15u;
        t->cpu.r[31] = IDLE_PC;
        t->cpu.r[28] = p.r[28];
        t->cpu.vctl[0] = t->cpu.vctl[1] = 0xE4;
        return i;
    }
    return -1;
}

#include "psp_interp.c"
#include "psp_audio.c"
#include "psp_intr.c"
#include "psp_codec.c"
#include "psp_save.c"

static struct { int on; char id[64]; u32 crc; char dirs[2][1024]; } packaged;  /* see window_packaged */

#ifdef PSP_WINDOW
#include "psp_window.c"
#else
static void alert(const char *message) { puts(message); }
#endif

#define PUT_RETURN 0x08000020u
static u32 put_return[MAX_THREADS];

static void schedule(void) {
    if (in_intr) return;
    th[cur].cpu = p; th[cur].intr_off = intr_off; intr_off = 0;      /* a wait lets interrupts in until the thread is back */
    for (;;) {
        int best = -1;
        for (int k = 1; k <= MAX_THREADS; k++) {     /* round robin among equals */
            const int i = (cur + k) % MAX_THREADS;
            Thread *t = &th[i];
            if (t->state != FREE && t->state != DONE && t->state != READY && t->timed && t->until <= now_us) {
                if (t->state != WAIT_TIME) t->cpu.r[2] = 0x800201A8u;      /* a sleep ends well, anything else timed out */
                t->state = READY; t->timed = 0;
            }
            if (t->state == READY && (best < 0 || t->prio < th[best].prio)) best = i;
        }
        if (best >= 0) { cur = best; p = th[cur].cpu; intr_off = th[cur].intr_off; return; }
        u64 soon = ~0ull;
        for (int i = 0; i < MAX_THREADS; i++) if (th[i].state != FREE && th[i].state != DONE && th[i].timed && th[i].until < soon) soon = th[i].until;
        const u64 intr = intr_next();
        if (intr < soon) soon = intr;
        if (soon == ~0ull) { stop_reason = "every thread is waiting and nothing will wake them"; return; }
        now_us = soon;
        psp_tick();
    }
}
/* a more urgent thread whose wait just ended takes over, as the sound thread must */
static int urgent_due(void) {
    if (intr_off) return 0;
    for (int i = 0; i < MAX_THREADS; i++) if (th[i].timed && th[i].state != FREE && th[i].state != DONE && th[i].until <= now_us && th[i].prio < th[cur].prio) return 1;
    return 0;
}
static void wait_for(int state, int id, int need, int mode, u64 us) {
    Thread *t = &th[cur];
    if (in_intr) return;
    t->state = state; t->id = id; t->need = need; t->mode = mode;
    t->timed = us != 0; t->until = now_us + us;
    p.r[2] = 0;
    schedule();
}

/* a disc takes time: the caller keeps its return value and lets lower priorities run */
static void io_wait(u32 bytes) {
    Thread *t = &th[cur];
    t->state = WAIT_TIME; t->timed = 1; t->until = now_us + 200 + bytes / 2;      /* about 2 MB/s */
    schedule();
}

/* --------------------------------------------------- semaphores and flags */
static struct { int used, count, max; } sema[256];
static int sema_ok(u32 id) { return (id & ~255u) == 0x3000 && (id & 255) && sema[id & 255].used; }
static struct { int used; u32 bits; } flag[256];
static void wake_sema(int s) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (th[i].state == WAIT_SEMA && th[i].id == s && sema[s].count >= th[i].need) { sema[s].count -= th[i].need; th[i].state = READY; th[i].timed = 0; }
}
static int flag_met(int f, u32 want, int mode) { return mode & 1 ? (flag[f].bits & want) != 0 : (flag[f].bits & want) == want; }
static void wake_flag(int f) {
    for (int i = 0; i < MAX_THREADS; i++) if (th[i].state == WAIT_FLAG && th[i].id == f && flag_met(f, (u32)th[i].need, th[i].mode)) {
        if (th[i].out) WR32(th[i].out, flag[f].bits);
        if (th[i].mode & 0x20) flag[f].bits = 0; else if (th[i].mode & 0x10) flag[f].bits &= ~(u32)th[i].need;
        th[i].state = READY; th[i].timed = 0;
    }
}

/* ----------------------------------------------------------------- disc */
static FILE *iso;
static u64 read_bytes; static u32 last_lba, last_pos;
static int dir_lookup(u32 lba, u32 size, const char *name, size_t n, u32 *out_lba, u32 *out_size) {
    static u8 b[0x20000];
    if (size > sizeof b) size = sizeof b;
    fseek(iso, (long)lba * 2048, SEEK_SET);
    size = (u32)fread(b, 1, size, iso);
    for (u32 o = 0; o < size;) {
        if (!b[o]) { o = (o / 2048 + 1) * 2048; continue; }
        const int len = b[o + 32]; const char *nm = (char *)b + o + 33;
        int l = len; for (int i = 0; i < len; i++) if (nm[i] == ';') { l = i; break; }
        if ((size_t)l == n && !strncasecmp(nm, name, n)) { memcpy(out_lba, b + o + 2, 4); memcpy(out_size, b + o + 10, 4); return 1; }
        o += b[o];
    }
    return 0;
}
static int iso_find(const char *path, u32 *lba, u32 *size) {
    static u8 pvd[2048];
    fseek(iso, 16 * 2048, SEEK_SET);
    fread(pvd, 1, 2048, iso);
    memcpy(lba, pvd + 158, 4); memcpy(size, pvd + 166, 4);
    while (*path) {
        while (*path == '/') path++;
        const char *e = path; while (*e && *e != '/') e++;
        if (e > path && !dir_lookup(*lba, *size, path, (size_t)(e - path), lba, size)) return 0;
        path = e;
    }
    return 1;
}
static struct { int used, sectors; u32 lba, size, pos; s64 async; } files[64];   /* a whole-disc file counts in sectors */
static int file_open(const char *path) {
    const char *c = strchr(path, ':');
    if (!c || strncmp(path, "disc0", 5) && strncmp(path, "umd", 3)) return -1;
    u32 lba, size;
    if (!c[1]) {                                       /* the raw disc */
        fseek(iso, 0, SEEK_END);
        const u32 total = (u32)(ftell(iso) / 2048);
        for (int i = 3; i < 64; i++) if (!files[i].used) { files[i].used = 1; files[i].sectors = 1; files[i].lba = 0; files[i].size = total; files[i].pos = 0; return i; }
        return -1;
    }
    if (!strncmp(c + 1, "/sce_lbn", 8)) { unsigned l = 0, s = 0; sscanf(c + 9, "0x%x_size0x%x", &l, &s); lba = l; size = s;
        if (!size) { fseek(iso, 0, SEEK_END); size = (u32)(ftell(iso) - (long)l * 2048); } }      /* size 0: the rest of the disc */
    else if (!iso_find(c + 1, &lba, &size)) return -1;
    for (int i = 3; i < 64; i++) if (!files[i].used) { files[i].used = 1; files[i].sectors = 0; files[i].lba = lba; files[i].size = size; files[i].pos = 0; return i; }
    return -1;
}
static s64 file_read(int f, u32 to, u32 n) {
    if (f < 3 || f >= 64 || !files[f].used) return (s64)(s32)0x80020323;
    if (n > files[f].size - files[f].pos) n = files[f].size - files[f].pos;
    fseek(iso, (long)files[f].lba * 2048 + (long)files[f].pos * (files[f].sectors ? 2048 : 1), SEEK_SET);
    n = (u32)fread(M(to), files[f].sectors ? 2048 : 1, n, iso);
    read_bytes += n; last_lba = files[f].lba; last_pos = files[f].pos;
    files[f].pos += n;
    return n;
}

/* ------------------------------------------------------ system functions */
#define A0 p.r[4]
#define A1 p.r[5]
#define A2 p.r[6]
#define A3 p.r[7]
#define T0 p.r[8]
#define T1 p.r[9]
#define RET(v) (p.r[2] = (u32)(v))
static const char *gstr(u32 a) { return a ? (const char *)M(a) : ""; }
static u32 clock_pll = 222, clock_cpu = 222, clock_bus = 111;
static int dialog = -1;
static u32 psmf_status, vcount, fb_addr, fb_stride, fb_fmt, ge_lists;
static u32 blocks_alloc[1024][2]; static int nalloc;
static u32 uid_alloc(u32 addr, u32 size) { blocks_alloc[nalloc][0] = addr; blocks_alloc[nalloc][1] = size; return 0x2000 + nalloc++; }
static u64 next_vblank(void) { const u64 f = 16683; return (now_us / f + 1) * f - now_us; }

static void hle(int i) {
    const char *n = imports[i].name;
    now_us += 3;           /* a system call takes time, so a thread polling through them sees the clock move */
    psp_tick();
    imports[i].calls++;
    RET(0);
    if (imports[i].nid == 0xEBD177D6u) { clock_pll = A0; clock_cpu = A1; clock_bus = A2; return; }   /* the 3-argument clock set */
    if (!n) return;
#define IS(s) (!strcmp(n, s))
    if (IS("sceKernelCreateThread")) { const int t = new_thread(gstr(A0), A1, (int)A2, A3); RET(t < 0 ? 0x80020064u : (u32)UID(t)); fprintf(lg, "thread %s entry %08X prio %d\n", gstr(A0), A1, (int)A2); }
    else if (IS("sceKernelStartThread")) {
        const int t = TID(A0);
        if (t < 0 || t >= MAX_THREADS) { RET(ERR_NOT_FOUND); return; }
        const u32 n2 = A1, src = A2;
        Thread *d = &th[t];
        u32 sp = d->cpu.r[29];
        if (n2 && src) { sp = (sp - n2 - 16) & ~15u; memcpy(M(sp), M(src), n2); d->cpu.r[5] = sp; d->cpu.r[29] = sp - 32; }
        d->cpu.r[4] = n2; d->state = READY;
        if (d->prio < th[cur].prio && !intr_off) { th[cur].state = READY; schedule(); }
    }
    else if (IS("sceKernelTerminateDeleteThread") || IS("sceKernelTerminateThread")) {      /* another thread: the caller carries on */
        const int t = TID(A0);
        if (t >= 0 && t < MAX_THREADS && t != cur) { if (IS("sceKernelTerminateThread")) th[t].state = DONE; else delete_thread(&th[t]); th[t].timed = 0; }
    }
    else if (IS("sceKernelExitThread") || IS("sceKernelExitDeleteThread")) {
        th[cur].state = DONE;
        for (int k = 0; k < MAX_THREADS; k++) if (th[k].state == WAIT_END && th[k].id == cur) { th[k].state = READY; th[k].timed = 0; }
        if (IS("sceKernelExitDeleteThread")) delete_thread(&th[cur]);
        schedule();
    }
    else if (IS("sceKernelDeleteThread")) { const int t = TID(A0); if (t >= 0 && t < MAX_THREADS) delete_thread(&th[t]); }
    else if (IS("sceKernelCpuSuspendIntr")) { RET(!intr_off); intr_off = 1; }
    else if (IS("sceKernelCpuResumeIntr") || IS("sceKernelCpuResumeIntrWithSync")) intr_off = !A0;
    else if (IS("sceKernelIsCpuIntrEnable")) RET(!intr_off);
    else if (IS("sceKernelIsCpuIntrSuspended")) RET(A0 == 0);         /* given what sceKernelCpuSuspendIntr returned */
    else if (IS("sceKernelMemset")) { memset(M(A0), (int)(A1 & 0xFF), A2); RET(A0); }
    else if (IS("sceKernelMemcpy")) { memmove(M(A0), M(A1), A2); RET(A0); }
    else if (IS("sceKernelGetThreadId")) RET(UID(cur));
    else if (IS("sceKernelSysClock2USec")) { const u64 c = (u64)RD32(A0 + 4) << 32 | RD32(A0); if (A1) WR32(A1, (u32)(c / 1000000)); if (A2) WR32(A2, (u32)(c % 1000000)); }
    else if (IS("sceKernelSysClock2USecWide")) { const u64 c = (u64)A1 << 32 | A0; if (A2) WR32(A2, (u32)(c / 1000000)); if (A3) WR32(A3, (u32)(c % 1000000)); }
    else if (IS("sceKernelChangeThreadPriority")) { const int t = A0 ? TID(A0) : cur; if (t >= 0 && t < MAX_THREADS) th[t].prio = (int)A1; }
    else if (IS("sceKernelGetThreadCurrentPriority")) RET(th[cur].prio);
    else if (IS("sceKernelDelayThread") || IS("sceKernelDelayThreadCB")) wait_for(WAIT_TIME, 0, 0, 0, A0 ? A0 : 1);
    else if (IS("sceKernelSleepThread") || IS("sceKernelSleepThreadCB")) { if (th[cur].wakeups) th[cur].wakeups--; else wait_for(WAIT_SLEEP, 0, 0, 0, 0); }
    else if (IS("sceKernelWakeupThread")) {             /* a wakeup with nobody asleep is kept for the next sleep */
        const int t = A0 ? TID(A0) : cur;
        if (t >= 0 && t < MAX_THREADS) { if (th[t].state == WAIT_SLEEP) th[t].state = READY; else th[t].wakeups++; }
    }
    else if (IS("sceKernelCancelWakeupThread")) { const int t = A0 ? TID(A0) : cur; if (t >= 0 && t < MAX_THREADS) { RET(th[t].wakeups); th[t].wakeups = 0; } }
    else if (IS("sceKernelWaitThreadEnd") || IS("sceKernelWaitThreadEndCB")) { const int t = TID(A0); if (t >= 0 && t < MAX_THREADS && th[t].state != DONE && th[t].state != FREE) wait_for(WAIT_END, t, 0, 0, 0); }
    else if (IS("sceKernelCreateSema")) { for (int s = 1; s < 256; s++) if (!sema[s].used) { sema[s].used = 1; sema[s].count = (int)A2; sema[s].max = (int)A3; RET(0x3000 + s); return; } RET(0x80020064u); }
    else if ((IS("sceKernelDeleteSema") || IS("sceKernelSignalSema") || IS("sceKernelPollSema") || IS("sceKernelWaitSema")
              || IS("sceKernelWaitSemaCB")) && !sema_ok(A0)) {
        fprintf(lg, "%s on unknown semaphore %08X ra %08X\n", n, A0, p.r[31]);
        RET(0x80020199u);                               /* SCE_KERNEL_ERROR_UNKNOWN_SEMID, as the PSP answers */
    }
    else if (IS("sceKernelDeleteSema")) {               /* whoever waits on it wakes with an error */
        const int s = A0 & 255;
        sema[s].used = 0;
        for (int i = 0; i < MAX_THREADS; i++)
            if (th[i].state == WAIT_SEMA && th[i].id == s) { th[i].state = READY; th[i].timed = 0; th[i].cpu.r[2] = 0x800201B5u; }
    }
    else if (IS("sceKernelSignalSema")) { const int s = A0 & 255; sema[s].count += (int)A1; wake_sema(s); }
    else if (IS("sceKernelPollSema")) { const int s = A0 & 255; if (sema[s].count >= (int)A1) sema[s].count -= (int)A1; else RET(0x800201A4u); }
    else if (IS("sceKernelWaitSema") || IS("sceKernelWaitSemaCB")) {
        const int s = A0 & 255;
        if (sema[s].count >= (int)A1) sema[s].count -= (int)A1;
        else wait_for(WAIT_SEMA, s, (int)A1, 0, A2 ? RD32(A2) : 0);
    }
    else if (IS("sceKernelCreateEventFlag")) { for (int f = 1; f < 256; f++) if (!flag[f].used) { flag[f].used = 1; flag[f].bits = A2; RET(0x4000 + f); return; } RET(0x80020064u); }
    else if (IS("sceKernelDeleteEventFlag")) flag[A0 & 255].used = 0;
    else if (IS("sceKernelSetEventFlag")) { flag[A0 & 255].bits |= A1; wake_flag(A0 & 255); }
    else if (IS("sceKernelClearEventFlag")) flag[A0 & 255].bits &= A1;
    else if (IS("sceKernelPollEventFlag") || IS("sceKernelWaitEventFlag") || IS("sceKernelWaitEventFlagCB")) {
        const int f = A0 & 255;
        if (flag_met(f, A1, (int)A2)) {
            if (A3) WR32(A3, flag[f].bits);
            if (A2 & 0x20) flag[f].bits = 0; else if (A2 & 0x10) flag[f].bits &= ~A1;
        } else if (IS("sceKernelPollEventFlag")) RET(0x800201AFu);
        else { const u32 to = T0 ? RD32(T0) : 0; const int want = (int)A1, mode = (int)A2; th[cur].out = A3; wait_for(WAIT_FLAG, f, want, mode, to); }
    }
    else if (IS("sceKernelAllocPartitionMemory")) { const u32 a = A2 == 1 ? alloc_high(A3) : alloc_low(A3); RET(uid_alloc(a, A3)); fprintf(lg, "alloc %s %u bytes at %08X\n", gstr(A1), A3, a); }
    else if (IS("sceKernelGetBlockHeadAddr")) RET(blocks_alloc[(A0 - 0x2000) & 1023][0]);
    else if (IS("sceKernelMaxFreeMemSize") || IS("sceKernelTotalFreeMemSize")) RET(stack_top - heap);
    else if (IS("sceIoOpen") || IS("sceIoOpenAsync")) {
        const int f = file_open(gstr(A0));
        fprintf(lg, "open %s = %d\n", gstr(A0), f);
        if (IS("sceIoOpenAsync")) { if (f >= 0) { files[f].async = f; RET(f); } else RET(0x80010002u); }
        else RET(f < 0 ? 0x80010002u : (u32)f);
    }
    else if (IS("sceIoDopen")) RET(save_dir_open(gstr(A0)));
    else if (IS("sceIoDread")) RET(save_dir_read(A0, A1));
    else if (IS("sceIoDclose")) RET(save_dir_close(A0));
    else if (IS("sceIoDevctl")) {                       /* a Memory Stick that's in, ready and has room */
        fprintf(lg, "devctl %s %08X\n", gstr(A0), A1);
        if (A1 == 0x02025806 && T0) WR32(T0, 1);        /* inserted */
        else if (A1 == 0x02025801 && T0) WR32(T0, 4);   /* ready */
        else if (A1 == 0x02425818 && A2) {              /* free space */
            const u32 s = RD32(A2);
            WR32(s, 0x10000); WR32(s + 4, 0x8000); WR32(s + 8, 0x8000); WR32(s + 12, 512); WR32(s + 16, 64);
        }
    }
    else if (IS("sceIoGetstat")) {
        const char *c = strchr(gstr(A0), ':');
        u32 lba, size;
        if (!c || !iso_find(c + 1, &lba, &size)) { RET(0x80010002u); fprintf(lg, "stat %s: not found\n", gstr(A0)); return; }
        memset(M(A1), 0, 88);
        WR32(A1, 0x2000 | 0444); WR32(A1 + 4, 0x20); WR32(A1 + 8, size);
        WR32(A1 + 64, lba);                     /* games find what's hidden on the disc from here */
        fprintf(lg, "stat %s = %u\n", gstr(A0), size);
    }
    else if (IS("sceIoRead")) {
        static int shown; const u32 pos = files[A0 & 63].pos, to = A1, want = A2;
        RET(file_read((int)A0, A1, A2));
        if (shown++ < 400) fprintf(lg, "%.2f read fd %d lba %X pos %X want %X got %d to %08X ra %08X\n", now_us / 1e6, (int)A0, files[A0 & 63].lba, pos, want, (int)p.r[2], to, p.r[31]);
        io_wait(want);
    }
    else if (IS("sceIoReadAsync")) { const u32 want = A2; files[A0 & 63].async = file_read((int)A0, A1, A2); io_wait(want); }
    else if (IS("sceIoLseek32")) { const int f = A0 & 63; files[f].pos = (A2 == 0 ? 0 : A2 == 1 ? files[f].pos : files[f].size) + A1; RET(files[f].pos); }
    else if (IS("sceIoLseek") || IS("sceIoLseekAsync")) {
        const int f = A0 & 63, w = (int)T0;
        static int shown; if (shown++ < 14) fprintf(lg, "seek fd %d off %X:%X whence %d ra %08X\n", f, A3, A2, w, p.r[31]);
        files[f].pos = (w == 0 ? 0 : w == 1 ? files[f].pos : files[f].size) + A2;
        if (IS("sceIoLseek")) { RET(files[f].pos); p.r[3] = 0; } else files[f].async = files[f].pos;
    }
    else if (IS("sceIoIoctl")) {
        fprintf(lg, "ioctl fd %d cmd %08X out %08X\n", (int)A0, A1, T0);
        if (A1 == 0x01020006u && T0) WR32(T0, files[A0 & 63].lba);          /* where the file starts on the disc */
        if (A1 == 0x01020007u && T0) { WR32(T0, files[A0 & 63].size); WR32(T0 + 4, 0); }
    }
    else if (IS("sceIoGetAsyncStat")) { const s64 r = files[A0 & 63].async; WR32(A2, (u32)r); WR32(A2 + 4, (u32)(r >> 32)); }
    else if (IS("sceIoClose")) { if (A0 < 64) files[A0].used = 0; }
    else if (IS("sceIoCloseAsync")) { if (A0 < 64) { files[A0].used = 0; files[A0].async = 0; } }
    else if (IS("sceIoWaitAsync") || IS("sceIoWaitAsyncCB") || IS("sceIoPollAsync")) { if (A1) { const s64 r = files[A0 & 63].async; WR32(A1, (u32)r); WR32(A1 + 4, (u32)(r >> 32)); } }
    else if (IS("sceUmdCheckMedium")) RET(1);
    else if (IS("sceUmdGetDriveStat")) RET(0x32);
    else if (IS("sceDisplaySetFrameBuf")) {
        fb_addr = A0; fb_stride = A1; fb_fmt = A2;
#ifdef PSP_WINDOW
        if (A0) window_flip(A0, (int)A1, (int)A2);
#endif
        const char *shots = getenv("PSP_SHOTS");
        static u32 flips;
        psp_ge_new_frame();
        if (shots && fb_addr && !(flips++ % 120)) {
            char path[512]; snprintf(path, sizeof path, "%s/f%06u.ppm", shots, flips);
            psp_ge_shot(path, fb_addr, (int)fb_stride, (int)fb_fmt);
        }
    }
    else if (IS("sceDisplayGetVcount")) RET(now_us / 16683);
    else if (IS("sceDisplayWaitVblankStart") || IS("sceDisplayWaitVblankStartCB") || IS("sceDisplayWaitVblank") || IS("sceDisplayWaitVblankCB")) { vcount++; wait_for(WAIT_TIME, 0, 0, 0, next_vblank()); }
    else if (IS("sceCtrlReadBufferPositive") || IS("sceCtrlPeekBufferPositive")) {
        memset(M(A0), 0, 16); M(A0)[8] = M(A0)[9] = 128;
        const char *press = getenv("PSP_PRESS");        /* tests: BUTTONS,ONSECOND... hold each for a moment */
        if (press) { static u32 mask; if (!mask) mask = strtoul(press, NULL, 16); if ((now_us / 500000) % 4 == 1) WR32(A0 + 4, mask); }
        WR32(A0, (u32)now_us);
#ifdef PSP_WINDOW
        WR32(A0 + 4, pad_buttons);
        M(A0)[8] = pad_x; M(A0)[9] = pad_y;
#endif
        RET(1);
    }
    else if (IS("sceMpegQueryMemSize")) RET(0x10000);
    else if (IS("sceMpegRingbufferQueryMemSize")) RET(A0 * (104 + 2048));
    else if (IS("sceMpegRingbufferConstruct")) {
        WR32(A0, A1); WR32(A0 + 4, 0); WR32(A0 + 8, 0); WR32(A0 + 12, A1); WR32(A0 + 16, 2048);
        WR32(A0 + 20, A2); WR32(A0 + 24, T0); WR32(A0 + 28, T1);
        mpeg_reset(A0, A1, A2);
    }
    else if (IS("sceMpegRingbufferAvailableSize")) RET(RD32(A0 + 12));
    else if (IS("sceMpegRingbufferPut")) {             /* the game's callback fetches the file, and may wait */
        const u32 ring = A0, cb = RD32(ring + 24), room = mp.packets - mp.wrote;
        u32 n = A1 < A2 ? A1 : A2;
        if (n > room) n = room;
        RET(0);
        if (cb && n) {
            put_return[cur] = p.pc;
            p.r[4] = RD32(ring + 20) + mp.wrote * 2048; p.r[5] = n; p.r[6] = RD32(ring + 28);
            p.r[31] = PUT_RETURN; p.pc = cb;
        }
    }
    else if (IS("sceMpegQueryStreamOffset")) { WR32(A2, __builtin_bswap32(RD32(A1 + 8))); }
    else if (IS("sceMpegQueryStreamSize")) { WR32(A1, __builtin_bswap32(RD32(A0 + 12))); }
    else if (IS("sceMpegRegistStream")) RET(1);
    else if (IS("sceMpegQueryAtracEsSize")) { if (A1) WR32(A1, 2112); if (A2) WR32(A2, 8192); }
    else if (IS("sceMpegGetAtracAu")) RET(mpeg_next_audio(A2));
    else if (IS("sceMpegAtracDecode")) { mpeg_decode_audio(A2); if (A3) WR32(A3, 0); }
    else if (IS("sceMpegGetAvcAu") || IS("sceMpegGetAvcEsAu")) { const u32 attr = A3; RET(mpeg_next_video(A2)); if (!p.r[2] && attr) WR32(attr, 1); }
    else if (IS("sceMpegAvcDecode")) { const int shown = mpeg_decode_video((int)A2, RD32(A3)); if (T0) WR32(T0, (u32)shown); }
    else if (IS("sceMpegAvcDecodeDetail")) {
        WR32(A1, 1); WR32(A1 + 4, mp.decoded > 0); WR32(A1 + 8, (u32)mp.width); WR32(A1 + 12, (u32)mp.height);
        WR32(A1 + 16, 0); WR32(A1 + 20, 0); WR32(A1 + 24, 0); WR32(A1 + 28, 0);
    }
    else if (IS("sceKernelLoadModule")) {                 /* a module that's on the disc loads; its functions are ours */
        const char *c = strchr(gstr(A0), ':'); u32 lba, size;
        const int found = c && !strncmp(gstr(A0), "disc0", 5) && iso_find(c + 1, &lba, &size);
        fprintf(lg, "load module %s: %s\n", gstr(A0), found ? "ok" : "not found");
        RET(found ? 0x5000 + lba % 4096 : 0x80010002u);
    }
    else if (IS("sceKernelVolatileMemLock") || IS("sceKernelVolatileMemTryLock")) { if (A1) WR32(A1, 0x08400000); if (A2) WR32(A2, 0x400000); }
    else if (IS("sceAtracSetDataAndGetID")) RET(atrac_set_data(A0, A1));
    else if (IS("sceAtracReleaseAtracID")) { struct Atrac *a = atrac_of(A0); if (a) atrac_release(a); }
    else if (IS("sceAtracSetLoopNum")) { struct Atrac *a = atrac_of(A0); if (a) a->loops = (int)A1; }
    else if (IS("sceAtracGetRemainFrame")) { struct Atrac *a = atrac_of(A0); if (a) WR32(A1, atrac_remain(a)); }
    else if (IS("sceAtracAddStreamData")) { struct Atrac *a = atrac_of(A0); if (a) atrac_add(a, A1); }
    else if (IS("sceAtracGetStreamDataInfo")) {
        struct Atrac *a = atrac_of(A0); u32 write = 0, room = 0, from = 0;
        if (a) atrac_stream_info(a, &write, &room, &from);
        if (A1) WR32(A1, write);
        if (A2) WR32(A2, room);
        if (A3) WR32(A3, from);
    }
    else if (IS("sceAtracDecodeData")) {
        struct Atrac *a = atrac_of(A0); u32 finished = 1;
        const u32 made = a ? atrac_decode(a, A1, &finished) : 0x80630005u;
        if (A2) WR32(A2, made >> 31 ? 0 : made);
        if (A3) WR32(A3, finished);
        if (T0) WR32(T0, a ? atrac_remain(a) : 0);
        RET(made >> 31 ? made : 0);
    }
    else if (IS("__sceSasGetEndFlag")) RET(sas_end_flags());
    else if (IS("__sceSasInit")) { sas_grain = (int)A1; }
    else if (IS("__sceSasCore")) sas_mix(A1);
    else if (IS("__sceSasSetVoice") && A1 < SAS_VOICES) { voices[A1].vag = A2; voices[A1].size = A3; voices[A1].loop = (int)T0; }
    else if (IS("__sceSasSetPitch") && A1 < SAS_VOICES) voices[A1].pitch = (int)A2;
    else if (IS("__sceSasSetVolume") && A1 < SAS_VOICES) { voices[A1].left = abs((int)A2); voices[A1].right = abs((int)A3); }
    else if (IS("__sceSasSetKeyOn") && A1 < SAS_VOICES) sas_key_on((int)A1);
    else if (IS("__sceSasSetKeyOff") && A1 < SAS_VOICES) voices[A1].releasing = 1;
    else if (IS("sceAudioChReserve")) RET(audio_reserve((int)A0, (int)A1, (int)A2));
    else if (IS("sceAudioChRelease")) { if (A0 < AUDIO_CHANNELS) chans[A0].reserved = 0; }
    else if (IS("sceAudioSetChannelDataLen")) { if (A0 < AUDIO_CHANNELS) chans[A0].samples = (int)A1; }
    else if (IS("sceKernelGetSystemTime")) { WR32(A0, (u32)now_us); WR32(A0 + 4, (u32)(now_us >> 32)); }
    else if (IS("sceAtracGetMaxSample")) { if (A1) WR32(A1, 2048); }
    else if (IS("sceAtracGetNextSample")) { struct Atrac *a = atrac_of(A0); if (A1) WR32(A1, a && a->at + a->block <= a->end ? 2048 : 0); }
    else if (IS("sceUtilityOskInitStart")) osk_start(A0);
    else if (IS("sceUtilityOskGetStatus")) RET(dialog_status(&osk_status));
    else if (IS("sceUtilityOskUpdate")) { if (osk_status == 2) osk_status = 3; }
    else if (IS("sceUtilityOskShutdownStart")) osk_status = 4;
    else if (IS("sceUtilitySavedataInitStart")) save_start(A0);
    else if (IS("sceUtilitySavedataGetStatus")) RET(dialog_status(&save_status));
    else if (IS("sceUtilitySavedataUpdate")) { if (save_status == 2) save_status = 3; }
    else if (IS("sceUtilitySavedataShutdownStart")) save_status = 4;
    else if (IS("sceUtilityMsgDialogInitStart")) {
        dialog = 1;
        fprintf(lg, "dialog size %X mode %X flags %X error %X:", RD32(A0), RD32(A0 + 0x30), RD32(A0 + 0x38), RD32(A0 + 0x34));
        for (u32 o = 0x30; o < 0x280; o++) { const u8 c = RD8(A0 + o); if (c >= 32 && c < 127) fputc(c, lg); }
        fputc('\n', lg);
    }
    else if (IS("sceUtilityMsgDialogGetStatus")) RET(dialog_status(&dialog));
    else if (IS("sceUtilityMsgDialogUpdate")) { if (dialog == 2) dialog = 3; }
    else if (IS("sceUtilityMsgDialogShutdownStart")) dialog = 4;
    else if (IS("sceKernelGetSystemTimeLow") || IS("sceKernelLibcClock")) RET(now_us);
    else if (IS("sceKernelGetSystemTimeWide")) { RET(now_us); p.r[3] = (u32)(now_us >> 32); }
    else if (IS("sceRtcGetCurrentTick")) { WR32(A0, (u32)now_us); WR32(A0 + 4, (u32)(now_us >> 32)); }
    else if (IS("sceRtcGetTickResolution")) RET(1000000);
    else if (IS("sceGeEdramGetAddr")) RET(0x04000000);
    else if (IS("sceGeEdramGetSize")) RET(0x200000);
    else if (IS("sceGeListEnQueue") || IS("sceGeListEnQueueHead")) { ge_lists++; RET(psp_ge_enqueue(A0, A1)); }
    else if (IS("sceGeListUpdateStall") || IS("sceGeListUpdateStallAddr")) psp_ge_update_stall(A0, A1);
    else if (IS("sceGeSetCallback")) { ge_signal_fn = RD32(A0); ge_signal_arg = RD32(A0 + 4); ge_finish_fn = RD32(A0 + 8); ge_finish_arg = RD32(A0 + 12); RET(1); }
    else if (IS("sceGeUnsetCallback")) ge_signal_fn = ge_finish_fn = 0;
    else if (IS("sceKernelRegisterSubIntrHandler")) { if (A0 == 30 && A1 < 32) { vblank_sub[A1].fn = A2; vblank_sub[A1].arg = A3; } }
    else if (IS("sceKernelEnableSubIntr")) { if (A0 == 30 && A1 < 32) vblank_sub[A1].on = 1; }
    else if (IS("sceKernelDisableSubIntr")) { if (A0 == 30 && A1 < 32) vblank_sub[A1].on = 0; }
    else if (IS("sceKernelReleaseSubIntrHandler")) { if (A0 == 30 && A1 < 32) vblank_sub[A1].fn = 0; }
    else if (IS("sceKernelSetAlarm")) RET(set_alarm(A0, A1, A2));
    else if (IS("sceKernelSetSysClockAlarm")) RET(set_alarm(RD32(A0) | (u64)RD32(A0 + 4) << 32, A1, A2));
    else if (IS("sceKernelCancelAlarm")) { const u32 a = A0 - 0x5000u; if (a < 16) alarms[a].fn = 0; }
    else if (IS("scePsmfPlayerCreate")) psmf_status = 1;
    else if (IS("scePsmfPlayerSetPsmf") || IS("scePsmfPlayerSetPsmfCB")) { psmf_status = 2; fprintf(lg, "movie %s\n", gstr(A1)); }
    else if (IS("scePsmfPlayerStart")) psmf_status = 4;
    else if (IS("scePsmfPlayerUpdate")) { if (psmf_status == 4) psmf_status = 0x200; }      /* no video decoder yet: a movie ends at once */
    else if (IS("scePsmfPlayerGetCurrentStatus")) RET(psmf_status);
    else if (IS("scePsmfPlayerStop") || IS("scePsmfPlayerBreak")) psmf_status = 2;
    else if (IS("scePsmfPlayerReleasePsmf")) psmf_status = 1;
    else if (IS("scePsmfPlayerDelete")) psmf_status = 0;
    else if (IS("scePowerGetCpuClockFrequencyInt")) RET(clock_cpu);
    else if (IS("scePowerGetPllClockFrequencyInt")) RET(clock_pll);
    else if (IS("scePowerGetBusClockFrequencyInt")) RET(clock_bus);
    else if (IS("scePowerSetClockFrequency")) { clock_pll = A0; clock_cpu = A1; clock_bus = A2; }
    else if (IS("sceAudioOutputPannedBlocking")) { const u32 us = audio_output((int)A0, (int)A1, (int)A2, A3); if (us) wait_for(WAIT_TIME, 0, 0, 0, us); }
    else if (IS("sceAudioOutputBlocking")) { const u32 us = audio_output((int)A0, (int)A1, (int)A1, A2); if (us) wait_for(WAIT_TIME, 0, 0, 0, us); }
    else if (IS("sceKernelExitGame")) stop_reason = "the game exited";
}

/* ------------------------------------------------- the program, as the PSP loads it */
static u32 rd32(const u8 *b) { u32 v; memcpy(&v, b, 4); return v; }
static u16 rd16(const u8 *b) { u16 v; memcpy(&v, b, 2); return v; }

static u32 crc32_of(const u8 *b, u32 n) {
    u32 c = 0xFFFFFFFFu;
    while (n--) { c ^= *b++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

/* An ELF goes where it says. A PRX, which early games use, is placed where the
 * PSP's loader would put it, with its addresses fixed up for that. Gives the entry,
 * the bytes loaded (start, count: what the code was recompiled from) and the end. */
#define PRX_BASE 0x08804000u
static int program_load(const u8 *d, u32 n, u32 *entry, u32 *start, u32 *count, u32 *end) {
    if (n < 52 || memcmp(d, "\x7f" "ELF", 4)) return 0;
    const u32 phoff = rd32(d + 28), shoff = rd32(d + 32);
    const u16 type = rd16(d + 16), phnum = rd16(d + 44), shnum = rd16(d + 48);
    if (type != 0xFFA0) {
        if (phoff + 32 > n) return 0;
        const u8 *ph = d + phoff;
        const u32 off = rd32(ph + 4), va = rd32(ph + 8), fsz = rd32(ph + 16), msz = rd32(ph + 20);
        if (off + fsz > n || fsz > msz || va < 0x08800000u || va + msz > 0x0A000000u) return 0;
        memset(M(va), 0, msz);
        memcpy(M(va), d + off, fsz);
        *entry = rd32(d + 24); *start = va; *count = fsz; *end = va + msz;
        return 1;
    }
    if (!phnum || phnum > 8 || phoff + phnum * 32u > n) return 0;
    u32 vaddr[8], loaded = 0, size = 0;
    for (int i = 0; i < phnum; i++) {
        const u8 *ph = d + phoff + i * 32;
        const u32 off = rd32(ph + 4), fsz = rd32(ph + 16), msz = rd32(ph + 20);
        vaddr[i] = rd32(ph + 8);
        if (off + fsz > n || vaddr[i] + msz > 0x01000000u) return 0;
        if (vaddr[i] + fsz > loaded) loaded = vaddr[i] + fsz;
        if (vaddr[i] + msz > size) size = vaddr[i] + msz;
    }
    memset(M(PRX_BASE), 0, size);
    for (int i = 0; i < phnum; i++) {
        const u8 *ph = d + phoff + i * 32;
        memcpy(M(PRX_BASE + vaddr[i]), d + rd32(ph + 4), rd32(ph + 16));
    }
    for (int i = 0; i < shnum && shoff + (i + 1) * 40u <= n; i++) {
        const u8 *sh = d + shoff + i * 40;
        if (rd32(sh + 4) != 0x700000A0u) continue;
        const u32 at0 = rd32(sh + 16), len = rd32(sh + 20) / 8;
        if (at0 + len * 8 > n) return 0;
        const u8 *rel = d + at0;
        for (u32 r = 0; r < len; r++) {
            const u32 offset = rd32(rel + r * 8), info = rd32(rel + r * 8 + 4);
            const u32 kind = info & 0xFF, from = info >> 8 & 0xFF, to_seg = info >> 16 & 0xFF;
            if (from >= phnum || to_seg >= phnum || vaddr[from] + offset + 4 > size) continue;
            u8 *w = M(PRX_BASE + vaddr[from] + offset);
            u32 op = rd32(w);
            const u32 to = PRX_BASE + vaddr[to_seg];
            if (kind == 2) op += to;                                                        /* R_32 */
            else if (kind == 4) op = (op & 0xFC000000u) | ((((op & 0x03FFFFFFu) << 2) + to) >> 2 & 0x03FFFFFFu);
            else if (kind == 5) {                       /* R_HI16: the carry comes from the LO16 that follows */
                s32 lo = 0;
                for (u32 later = r + 1; later < len && later <= r + 63; later++) {
                    const u32 li = rd32(rel + later * 8 + 4);
                    if ((li & 0xFF) != 6) continue;
                    const u32 lf = li >> 8 & 0xFF;
                    if (lf < phnum) lo = (s16)(rd32(M(PRX_BASE + vaddr[lf] + rd32(rel + later * 8))) & 0xFFFF);
                    break;
                }
                const u32 full = ((op & 0xFFFF) << 16) + (u32)lo + to;
                op = (op & 0xFFFF0000u) | ((full + 0x8000) >> 16 & 0xFFFF);
            }
            else if (kind == 6) op = (op & 0xFFFF0000u) | ((op + to) & 0xFFFF);           /* R_LO16 */
            else continue;
            memcpy(w, &op, 4);
        }
    }
    *entry = PRX_BASE + rd32(d + 24); *start = PRX_BASE; *count = loaded; *end = PRX_BASE + size;
    return 1;
}

static u8 *file_all(FILE *f, u32 *n) {
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size <= 0 || size > 64 << 20) return NULL;
    u8 *b = malloc((size_t)size);
    fseek(f, 0, SEEK_SET);
    if (fread(b, 1, (size_t)size, f) != (size_t)size) { free(b); return NULL; }
    *n = (u32)size;
    return b;
}

static u8 *disc_file(const char *path, u32 *n) {
    u32 lba, size;
    if (!iso_find(path, &lba, &size) || !size || size > 64 << 20) return NULL;
    u8 *b = malloc(size);
    fseek(iso, (long)lba * 2048, SEEK_SET);
    if (fread(b, 1, size, iso) != size) { free(b); return NULL; }
    *n = size;
    return b;
}

/* the disc's ID, as PARAM.SFO gives it: ULUS10007 */
static int disc_id(char out[16]) {
    u32 n = 0; u8 *s = disc_file("PSP_GAME/PARAM.SFO", &n);
    int found = 0;
    if (s && n >= 20 && !memcmp(s, "\0PSF", 4)) {
        const u32 keys = rd32(s + 8), data = rd32(s + 12), count = rd32(s + 16);
        for (u32 i = 0; i < count && 20 + (i + 1) * 16 <= n && !found; i++) {
            const u8 *e = s + 20 + i * 16;
            const u32 key = keys + rd16(e), at = data + rd32(e + 12), len = rd32(e + 4);
            if (key + 8 <= n && !memcmp(s + key, "DISC_ID", 8) && at + len <= n && len > 1 && len < 16) {
                memcpy(out, s + at, len); out[len] = 0; found = 1;
            }
        }
    }
    free(s);
    return found;
}

/* A packaged game: the user's disc, found beside the player by its ID, and the program
 * on it, which must be the one the code was recompiled from. */
static struct { u32 entry, start, count, end; int wrong_version, needs_dump; char version_disc[1100]; } found;
static const char *file_name(const char *path) {
    const char *n = strrchr(path, '/');
#ifdef _WIN32
    if (strrchr(path, '\\') > n) n = strrchr(path, '\\');
#endif
    return n ? n + 1 : path;
}

/* a dump of the program the user made from their copy, under any name, known by its checksum */
static int dump_try(const char *path, void *unused) {
    (void)unused;
    const size_t l = strlen(path);
    if (l < 4 || strcasecmp(path + l - 4, ".bin") && strcasecmp(path + l - 4, ".elf")) return 0;
    FILE *f = psp_fopen(path, "rb");
    if (!f) return 0;
    u32 n = 0; u8 *prog = file_all(f, &n);
    fclose(f);
    int ok = prog && program_load(prog, n, &found.entry, &found.start, &found.count, &found.end)
             && (!packaged.crc || crc32_of(M(found.start), found.count) == packaged.crc);
    free(prog);
    return ok;
}

static int packaged_try(const char *path, void *unused) {
    (void)unused;
    const size_t l = strlen(path);
    if (l < 4 || strcasecmp(path + l - 4, ".iso")) return 0;
    iso = psp_fopen(path, "rb");
    if (!iso) return 0;
    char id[16];
    if (disc_id(id) && !strcmp(id, packaged.id)) {
        u32 n = 0; u8 *prog = disc_file("PSP_GAME/SYSDIR/BOOT.BIN", &n);
        if (!prog || n < 4 || memcmp(prog, "\x7f" "ELF", 4)) {                  /* encrypted: the user's own dump beside it */
            free(prog); prog = NULL;
            char dir[1100]; snprintf(dir, sizeof dir, "%s", path);
            char *slash = strrchr(dir, '/');
#ifdef _WIN32
            if (strrchr(dir, '\\') > slash) slash = strrchr(dir, '\\');
#endif
            if (slash) slash[1] = 0; else dir[0] = 0;
            if (dir[0] && psp_each_file(dir, dump_try, NULL)) return 1;
            found.needs_dump = 1;
            snprintf(found.version_disc, sizeof found.version_disc, "%s", file_name(path));
            fclose(iso); iso = NULL;
            return 0;
        }
        int ok = prog && program_load(prog, n, &found.entry, &found.start, &found.count, &found.end);
        free(prog);
        if (ok && (!packaged.crc || crc32_of(M(found.start), found.count) == packaged.crc)) return 1;
        found.wrong_version = 1;
        snprintf(found.version_disc, sizeof found.version_disc, "%s", file_name(path));
    }
    fclose(iso); iso = NULL;
    return 0;
}

static int packaged_start(u32 *entry, u32 *end) {
    for (int i = 0; i < 2; i++)
        if (packaged.dirs[i][0] && psp_each_file(packaged.dirs[i], packaged_try, NULL)) { *entry = found.entry; *end = found.end; return 1; }
    char message[2600];
    if (found.needs_dump)
        snprintf(message, sizeof message, "The disc %s holds its program encrypted.\n\nPut the decrypted program you dumped from it "
                 "(PPSSPP's %s_EBOOT.BIN, any name ending in .bin or .elf) in the same folder.", found.version_disc, packaged.id);
    else if (found.wrong_version)
        snprintf(message, sizeof message, "The disc %s is %s, but not the release this was made from. "
                 "Use the same release of the disc Genexia recompiled.", found.version_disc, packaged.id);
    else
        snprintf(message, sizeof message, "Put your copy of the game's disc, as an .iso file (%s), in this folder:\n\n%s",
                 packaged.id, packaged.dirs[1][0] ? packaged.dirs[1] : packaged.dirs[0]);
    alert(message);
    return 0;
}

int main(int argc, char **argv) {
#ifdef PSP_WINDOW
    if (argc < 3) window_packaged(&argc, &argv);        /* a packaged game: everything beside the player */
#endif
    if (argc < 3) return 1;
#ifdef PSP_WINDOW
    window_open("Genexia");
#endif
    psp_mem = psp_reserve(0x20000000);
    if (!psp_mem) { puts("not enough memory for the PSP's"); return 1; }
    if (getenv("PSP_FPS") && atoi(getenv("PSP_FPS")) > 0) vblank_us = VBLANK_US * 30 / atoi(getenv("PSP_FPS"));
    next_vblank_at = vblank_us;
    blocks = calloc(CODE_SLOTS, sizeof *blocks);
    import_of = calloc(CODE_SLOTS, sizeof *import_of);
    u32 entry = 0, end = 0;
    if (packaged.on) {
        if (!packaged_start(&entry, &end)) return 1;
    } else {
        iso = psp_fopen(argv[2], "rb");
        FILE *f = psp_fopen(argv[1], "rb");
        if (!f || !iso) { puts("can't open the program or the disc"); return 1; }
        u32 n = 0, start, count; u8 *prog = file_all(f, &n);
        fclose(f);
        if (!prog || !program_load(prog, n, &entry, &start, &count, &end)) { puts("not a program the player can load"); return 1; }
        free(prog);
    }
    heap = (end + 0xFFF) & ~0xFFFu;
    psp_register_game();
#ifdef _WIN32
    lg = stdout;
#else
    lg = fopen("/dev/stdout", "w");
#endif
    const double limit = argc > 3 ? atof(argv[3]) : 30;

    const int m = new_thread("main", entry, 32, 0x40000);
    th[m].state = READY;
    cur = m;
    p = th[m].cpu;
    p.vctl[0] = p.vctl[1] = 0xE4;
    const u32 arg = p.r[29] + 16;
    static const char path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    memcpy(M(arg), path, sizeof path);
    p.r[4] = sizeof path; p.r[5] = arg; p.r[29] -= 64;

    const clock_t start = clock();
    const u32 brk = getenv("PSP_BREAK") ? strtoul(getenv("PSP_BREAK"), NULL, 16) : 0; int brk_n = 0;
    const u64 brk_after = getenv("PSP_AFTER") ? strtoull(getenv("PSP_AFTER"), NULL, 10) * 1000000 : 0;
    while (!stop_reason) {
        const u32 pc = p.pc;
        if (pc == IDLE_PC) { th[cur].state = DONE; schedule(); continue; }
        if (pc == PUT_RETURN) {                         /* v0 is already its answer */
            p.pc = put_return[cur];
            mpeg_put_done((s32)p.r[2]);
            continue;
        }
        const u32 slot = (pc - CODE_BASE) >> 2;
        if (slot >= CODE_SLOTS) { static char t[64]; snprintf(t, sizeof t, "jump outside the program: %08X, ra %08X", pc, p.r[31]); stop_reason = t; break; }
        if (import_of[slot]) { p.pc = p.r[31]; hle(import_of[slot] - 1); if (urgent_due()) schedule(); continue; }
        if (pc == brk && brk_n < 30 && now_us > brk_after) {
            brk_n++;
            fprintf(lg, "break %08X t %.2f thread %s v0 %08X v1 %08X a0 %08X a1 %08X a2 %08X a3 %08X s0 %08X s1 %08X ra %08X sp %08X\n", pc, now_us / 1e6, th[cur].name, p.r[2], p.r[3], A0, A1, A2, A3, p.r[16], p.r[17], p.r[31], p.r[29]);
            fprintf(lg, "   s1+: %08X %08X %08X %08X %08X %08X\n", RD32(p.r[17]), RD32(p.r[17] + 4), RD32(p.r[17] + 8), RD32(p.r[17] + 12), RD32(p.r[17] + 16), RD32(p.r[17] + 20));
            for (int r = 16; r < 24; r++) { const u32 o = p.r[r]; if (o >> 24 < 8 || o >> 24 > 9) continue; const u32 vt = RD32(o); if (vt >> 24 != 8) continue; const u32 ti = RD32(vt - 4); if (ti >> 24 == 8) { const u32 nm = RD32(ti + 4); if (nm >> 24 == 8) fprintf(lg, "   s%d is %.60s\n", r - 16, (const char *)M(nm)); } }
            fprintf(lg, "   stack objects:");
            for (u32 s = 0, a = p.r[29]; s < 0x400; s += 4) { const u32 o = RD32(a + s); if (o >> 24 < 8 || o >> 24 > 9 || (o & 3)) continue; const u32 vt = RD32(o); if (vt >> 24 != 8 || (vt & 3)) continue; const u32 ti = RD32(vt - 4); if (ti >> 24 == 8 && !(ti & 3)) { const u32 nm = RD32(ti + 4); if (nm >> 24 == 8 && RD8(nm) >= '0' && RD8(nm) <= '9') fprintf(lg, " %.32s", (const char *)M(nm)); } }
            fputc('\n', lg);
            fprintf(lg, "   calls:");
            for (u32 s = 0, a = p.r[29]; s < 0x300; s += 4) { const u32 v = RD32(a + s); if (v >= 0x08804000 && v < 0x08BFA000 && !(v & 3) && RD32(v - 8) >> 26 == 3) fprintf(lg, " %08X", v); }
            fputc('\n', lg);
        }
        p.pc = blocks[slot] ? blocks[slot]() : psp_interp(pc);
        p.r[0] = 0;
        ran++;
        if (!(ran & 63)) now_us++;
        if (!(ran & 0xFFFFF)) {
            if ((double)(clock() - start) / CLOCKS_PER_SEC > limit) stop_reason = "time limit";
            if (!intr_off) {
                psp_tick();
                th[cur].state = READY;     /* a thread polling memory still has to let others run */
                schedule();
            }
        }
    }
    fflush(lg);
    printf("\nstopped: %s\nthread %s, pc %08X\n%llu blocks, %.2f virtual seconds, %u vblanks, %u GE lists, frame buffer %08X stride %u format %u\n",
           stop_reason, th[cur].name, p.pc, (unsigned long long)ran, now_us / 1e6, vcount, ge_lists, fb_addr, fb_stride, fb_fmt);
    for (int i = 0; i < MAX_THREADS; i++) if (th[i].state != FREE)
        printf("  thread %-24s state %d id %d pc %08X ra %08X prio %d\n", th[i].name, th[i].state, th[i].id, i == cur ? p.pc : th[i].cpu.pc, i == cur ? p.r[31] : th[i].cpu.r[31], th[i].prio);
    printf("read %llu MB, last at lba %X pos %X\n", (unsigned long long)(read_bytes >> 20), last_lba, last_pos);
    if (getenv("PSP_TASKS")) for (u32 o = 0x08C00000; o < 0x0A000000; o += 4) {
        const u32 vt = RD32(o);
        if (vt < 0x08B00000 || vt > 0x08C00000 || (vt & 3)) continue;
        const u32 ti = RD32(vt - 4);
        if (ti < 0x08B00000 || ti > 0x08C00000 || (ti & 3)) continue;
        const u32 nm = RD32(ti + 4);
        if (nm < 0x08B00000 || nm > 0x08C00000 || RD8(nm) < '0' || RD8(nm) > '9') continue;
        printf("obj %08X %.48s  +64 %08X +68 %08X +116 %08X +180 %08X +196 %08X\n", o, (const char *)M(nm), RD32(o + 64), RD32(o + 68), RD32(o + 116), RD32(o + 180), RD32(o + 196));
    }
    int named = 0, unnamed = 0;
    for (int i = 0; i < nimports; i++) if (imports[i].calls) { if (imports[i].name) named++; else unnamed++; }
    printf("system functions called: %d named, %d not named\n", named, unnamed);
    for (int i = 0; i < nimports; i++) if (imports[i].calls)
        printf("  %8u  %-20s %08X %s\n", imports[i].calls, imports[i].module, imports[i].nid, imports[i].name ? imports[i].name : "(unknown)");
    printf("blocks found only while running: %llu\n", (unsigned long long)interpreted);
    printf("GE: %llu primitives, %llu triangles, %llu curved patches skipped\n", (unsigned long long)psp_ge_prims, (unsigned long long)psp_ge_tris, (unsigned long long)(psp_ge_unknown[5] + psp_ge_unknown[6]));
    printf("VFPU instructions without a core: %llu\n", (unsigned long long)psp_vfpu_unknown);
    for (int i = 0; i < psp_vfpu_nmissing; i++) printf("  %08X\n", psp_vfpu_missing[i]);
    return 0;
}

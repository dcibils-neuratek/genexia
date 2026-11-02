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
#include <stdlib.h>
#include <sys/mman.h>
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
    int state, prio, id, need, mode, wakeups; u32 out; u64 until; int timed; char name[32];
    PspCpu cpu;
} Thread;
#define MAX_THREADS 64
static Thread th[MAX_THREADS];
static int cur;
#define UID(i) (0x1000 + (i))
#define TID(u) ((int)(u) - 0x1000)
#define ERR_NOT_FOUND 0x80020198u

static u32 stack_top = 0x09FFFF00u, heap = 0;
static u32 alloc_low(u32 n) { u32 a = heap; heap = (heap + n + 63) & ~63u; return a; }
static u32 alloc_high(u32 n) { stack_top = (stack_top - n) & ~63u; return stack_top; }

static int new_thread(const char *name, u32 entry, int prio, u32 stack) {
    for (int i = 0; i < MAX_THREADS; i++) if (th[i].state == FREE) {
        Thread *t = &th[i];
        memset(t, 0, sizeof *t);
        snprintf(t->name, sizeof t->name, "%s", name);
        t->state = DONE;                    /* dormant until started */
        t->prio = prio;
        t->cpu.pc = entry;
        const u32 top = alloc_high(stack ? stack : 0x40000) + (stack ? stack : 0x40000);
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

#ifdef PSP_WINDOW
#include "psp_window.c"
#endif

#define PUT_RETURN 0x08000020u
static u32 put_return[MAX_THREADS], movie_frames;

static void schedule(void) {
    if (in_intr) return;
    th[cur].cpu = p;
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
        if (best >= 0) { cur = best; p = th[cur].cpu; return; }
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
static int dialog;
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
        if (d->prio < th[cur].prio) { th[cur].state = READY; schedule(); }
    }
    else if (IS("sceKernelTerminateDeleteThread") || IS("sceKernelTerminateThread")) {      /* another thread: the caller carries on */
        const int t = TID(A0);
        if (t >= 0 && t < MAX_THREADS && t != cur) { th[t].state = IS("sceKernelTerminateThread") ? DONE : FREE; th[t].timed = 0; }
    }
    else if (IS("sceKernelExitThread") || IS("sceKernelExitDeleteThread")) {
        th[cur].state = DONE;
        for (int k = 0; k < MAX_THREADS; k++) if (th[k].state == WAIT_END && th[k].id == cur) { th[k].state = READY; th[k].timed = 0; }
        schedule();
    }
    else if (IS("sceKernelDeleteThread")) { const int t = TID(A0); if (t >= 0 && t < MAX_THREADS) th[t].state = FREE; }
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
    else if (IS("sceKernelDeleteSema")) sema[A0 & 255].used = 0;
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
    /* movies: the stream is sized and accepted, and there's no decoder yet, so no frame ever comes */
    else if (IS("sceMpegQueryMemSize")) RET(0x10000);
    else if (IS("sceMpegRingbufferQueryMemSize")) RET(A0 * (104 + 2048));
    else if (IS("sceMpegRingbufferConstruct")) {
        WR32(A0, A1); WR32(A0 + 4, 0); WR32(A0 + 8, 0); WR32(A0 + 12, A1); WR32(A0 + 16, 2048);
        WR32(A0 + 20, A2); WR32(A0 + 24, T0); WR32(A0 + 28, T1);
    }
    else if (IS("sceMpegRingbufferAvailableSize")) RET(RD32(A0 + 12));
    else if (IS("sceMpegRingbufferPut")) {             /* the game's callback fetches the file, and may wait */
        const u32 ring = A0, cb = RD32(ring + 24), n = A1 < A2 ? A1 : A2;
        if (cb && n) {
            put_return[cur] = p.pc;
            p.r[4] = RD32(ring + 20); p.r[5] = n; p.r[6] = RD32(ring + 28);
            p.r[31] = PUT_RETURN; p.pc = cb;
        }
    }
    else if (IS("sceMpegQueryStreamOffset")) { WR32(A2, __builtin_bswap32(RD32(A1 + 8))); }
    else if (IS("sceMpegQueryStreamSize")) { WR32(A1, __builtin_bswap32(RD32(A0 + 12))); }
    else if (IS("sceMpegRegistStream")) RET(1);
    else if (IS("sceMpegGetAtracAu")) RET(0x80618001u);
    else if (IS("sceMpegGetAvcAu") || IS("sceMpegGetAvcEsAu")) {    /* blank frames, as many as the stream holds */
        if (!movie_frames) RET(0x80618001u);
        else {
            static u32 pts;
            movie_frames--; pts += 3003;
            WR32(A2, 0); WR32(A2 + 4, pts); WR32(A2 + 8, 0); WR32(A2 + 12, pts);
        }
    }
    else if (IS("sceMpegAvcDecode")) { if (T0) WR32(T0, 1); }
    else if (IS("sceKernelLoadModule")) {                 /* a module that's on the disc loads; its functions are ours */
        const char *c = strchr(gstr(A0), ':'); u32 lba, size;
        const int found = c && !strncmp(gstr(A0), "disc0", 5) && iso_find(c + 1, &lba, &size);
        fprintf(lg, "load module %s: %s\n", gstr(A0), found ? "ok" : "not found");
        RET(found ? 0x5000 + lba % 4096 : 0x80010002u);
    }
    else if (IS("sceKernelVolatileMemLock") || IS("sceKernelVolatileMemTryLock")) { if (A1) WR32(A1, 0x08400000); if (A2) WR32(A2, 0x400000); }
    else if (IS("sceAtracDecodeData")) {                 /* no decoder yet: music plays as silence */
        if (A1) memset(M(A1), 0, 2048 * 2);
        if (A2) WR32(A2, 1024);
        if (A3) WR32(A3, 0);
        if (T0) WR32(T0, 0xFFFFFFFFu);
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
    else if (IS("sceUtilityMsgDialogInitStart")) {
        dialog = 1;
        fprintf(lg, "dialog size %X mode %X flags %X error %X:", RD32(A0), RD32(A0 + 0x30), RD32(A0 + 0x38), RD32(A0 + 0x34));
        for (u32 o = 0x30; o < 0x280; o++) { const u8 c = RD8(A0 + o); if (c >= 32 && c < 127) fputc(c, lg); }
        fputc('\n', lg);
    }
    else if (IS("sceUtilityMsgDialogGetStatus")) { RET(dialog); if (dialog == 1) dialog = 2; else if (dialog == 4) dialog = 0; }
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

int main(int argc, char **argv) {
    if (argc < 3) return 1;
#ifdef PSP_WINDOW
    window_open("Genexia");
#endif
    psp_mem = mmap(NULL, 0x20000000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    blocks = calloc(CODE_SLOTS, sizeof *blocks);
    import_of = calloc(CODE_SLOTS, sizeof *import_of);
    iso = fopen(argv[2], "rb");
    FILE *f = fopen(argv[1], "rb");
    if (!f || !iso) { puts("can't open the program or the disc"); return 1; }
    static u8 h[0x1000];
    fread(h, 1, sizeof h, f);
    u32 entry, ph, off, va, fsz, msz;
    memcpy(&entry, h + 24, 4); memcpy(&ph, h + 28, 4);
    memcpy(&off, h + ph + 4, 4); memcpy(&va, h + ph + 8, 4); memcpy(&fsz, h + ph + 16, 4); memcpy(&msz, h + ph + 20, 4);
    fseek(f, off, SEEK_SET);
    fread(M(va), 1, fsz, f);
    fclose(f);
    heap = (va + msz + 0xFFF) & ~0xFFFu;
    psp_register_game();
    lg = fopen("/dev/stdout", "w");
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
            if ((s32)p.r[2] > 0) movie_frames += p.r[2] / 16;
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
            psp_tick();
            th[cur].state = READY;     /* a thread polling memory still has to let others run */
            schedule();
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

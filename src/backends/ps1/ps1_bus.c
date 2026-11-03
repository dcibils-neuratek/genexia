/*
 * ps1_bus.c - memory, the interrupt controller, DMA, the root counters, the
 * pad port and the event clock that ties them together.
 */
#include "ps1.h"

u8 ps1_ram[0x200000], ps1_bios[0x80000], ps1_scratch[0x400];
u64 ps1_now, ps1_next;
u16 pad_buttons;

static u32 i_stat, i_mask;
static u32 memctl[16], ram_size, cache_ctl;
static u64 events[EV_COUNT];

/* ---- events ---- */

static void line_event(void);
static void (*const handlers[EV_COUNT])(void) = { line_event, timers_event, cd_event, cd_sector_event, pad_event };

static void events_next(void) {
    u64 n = ~0ull;
    for (int i = 0; i < EV_COUNT; i++) if (events[i] < n) n = events[i];
    ps1_next = n;
}

void event_at(int id, u64 at) { events[id] = at; events_next(); }
void event_off(int id) { events[id] = ~0ull; events_next(); }

void events_run(void) {
    for (int i = 0; i < EV_COUNT; i++)
        if (events[i] <= ps1_now) { events[i] = ~0ull; events_next(); handlers[i](); }
}

/* ---- interrupts ---- */

void irq_update(void) {
    if (i_stat & i_mask) cpu.cause |= 0x400; else cpu.cause &= ~0x400u;
}

void irq_raise(int n) { i_stat |= 1u << n; irq_update(); }

/* ---- the screen's lines ---- */

static int line;
int ps1_frame_done;

static void line_event(void) {
    line = (line + 1) % FRAME_LINES;
    if (line == VISIBLE_LINES) { irq_raise(0); ps1_frame_done = 1; }
    gpu_line(line);
    timers_line(line, line >= VISIBLE_LINES);
    event_at(EV_LINE, ps1_now + LINE_CYCLES);
}

/* ---- root counters ---- */

static struct { u32 count, mode, target; u64 last; u32 frac; int fired; } tm[3];
static int in_vblank;

static int timer_runs(int n) {
    if (n == 2 && (tm[2].mode & 1) && ((tm[2].mode >> 1) & 3) % 3 == 0) return 0;   /* sync modes 0 and 3 stop it */
    return 1;
}

static int timer_sysclk(int n) {        /* counts CPU cycles, possibly divided */
    const int src = (tm[n].mode >> 8) & 3;
    if (n == 0) return !(src & 1);
    if (n == 1) return !(src & 1);
    return 1;
}

static void timer_tick(int n, u32 ticks) {
    if (!ticks || !timer_runs(n)) return;
    const u32 target = tm[n].target ? tm[n].target : 0x10000;
    u32 c = tm[n].count + ticks;
    int irq = 0;
    if (tm[n].count < target && c >= target) {
        tm[n].mode |= 1u << 11;
        if (tm[n].mode & 0x10) irq = 1;
        if (tm[n].mode & 0x08) c = target ? (c - target) % target : 0;
    }
    if (c >= 0x10000) {
        tm[n].mode |= 1u << 12;
        if (tm[n].mode & 0x20) irq = 1;
        c &= 0xffff;
    }
    tm[n].count = c;
    if (irq && (!tm[n].fired || (tm[n].mode & 0x40))) {
        tm[n].fired = 1;
        if (tm[n].mode & 0x80) tm[n].mode ^= 1u << 10; else tm[n].mode &= ~(1u << 10);
        irq_raise(4 + n);
        if (!(tm[n].mode & 0x80)) tm[n].mode |= 1u << 10;
    }
}

static void timer_sync(int n) {
    const u64 d = ps1_now - tm[n].last;
    tm[n].last = ps1_now;
    if (n == 0 && ((tm[0].mode >> 8) & 1)) {        /* the dot clock */
        const u64 t = d * 11 * 3413 / (7 * LINE_CYCLES) + tm[0].frac;   /* video clock is 11/7 the CPU's */
        tm[0].frac = 0;
        timer_tick(0, (u32)(t / (u32)gpu_dot_div));
        return;
    }
    if (!timer_sysclk(n)) return;
    if (n == 2 && (tm[2].mode & 0x200)) {
        const u64 t = d + tm[2].frac;
        tm[2].frac = t & 7;
        timer_tick(2, (u32)(t >> 3));
        return;
    }
    timer_tick(n, (u32)d);
}

static void timers_plan(void) {         /* wake for the next target or wrap */
    u64 at = ~0ull;
    for (int n = 0; n < 3; n++) {
        if (!timer_sysclk(n) || !timer_runs(n) || (n == 0 && ((tm[0].mode >> 8) & 1))) continue;
        if (!(tm[n].mode & 0x30) || (tm[n].fired && !(tm[n].mode & 0x40))) continue;
        const u32 target = tm[n].target ? tm[n].target : 0x10000;
        u32 left = tm[n].count < target && (tm[n].mode & 0x10) ? target - tm[n].count : 0x10000 - tm[n].count;
        if (!left) left = 1;
        const u64 cycles = n == 2 && (tm[2].mode & 0x200) ? (u64)left * 8 - tm[2].frac : left;
        if (ps1_now + cycles < at) at = ps1_now + cycles;
    }
    if (at != ~0ull) event_at(EV_TIMER, at); else event_off(EV_TIMER);
}

void timers_event(void) {
    for (int n = 0; n < 3; n++) timer_sync(n);
    timers_plan();
}

void timers_line(int l, int vblank) {
    for (int n = 0; n < 3; n++) timer_sync(n);
    if ((tm[1].mode >> 8) & 1) timer_tick(1, 1);       /* hblank */
    if (vblank != in_vblank) {
        in_vblank = vblank;
        if ((tm[1].mode & 1) && vblank) {
            const int sync = (tm[1].mode >> 1) & 3;
            if (sync == 1 || sync == 2) tm[1].count = 0;
        }
    }
    (void)l;
    timers_plan();
}

static u32 timer_read(int n, int reg) {
    timer_sync(n);
    if (reg == 0) return tm[n].count;
    if (reg == 1) { const u32 m = tm[n].mode; tm[n].mode &= ~(3u << 11); return m; }
    return tm[n].target;
}

static void timer_write(int n, int reg, u32 v) {
    timer_sync(n);
    if (reg == 0) tm[n].count = v & 0xffff;
    else if (reg == 1) { tm[n].mode = (v & 0x3ff) | (1u << 10); tm[n].count = 0; tm[n].fired = 0; tm[n].frac = 0; }
    else tm[n].target = v & 0xffff;
    timers_plan();
}

/* ---- DMA ---- */

static struct { u32 madr, bcr, chcr; } dma[7];
static u32 dpcr = 0x07654321, dicr;

static void dicr_update(void) {
    const int was = dicr >> 31;
    const u32 flags = (dicr >> 24) & 0x7f, enabled = (dicr >> 16) & 0x7f;
    const int now = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & enabled));
    dicr = (dicr & 0x7fffffff) | (u32)now << 31;
    if (now && !was) irq_raise(3);
}

static inline u32 ram32(u32 a) { u32 v; memcpy(&v, ps1_ram + (a & 0x1ffffc), 4); return v; }
static inline void ram32w(u32 a, u32 v) { memcpy(ps1_ram + (a & 0x1ffffc), &v, 4); }

static void dma_word(int ch, int to_ram, u32 a) {
    switch (ch) {
    case 0: if (!to_ram) mdec_dma_in(ram32(a)); break;
    case 1: if (to_ram) ram32w(a, mdec_dma_out()); break;
    case 2: if (to_ram) ram32w(a, gpu_read()); else gpu_gp0(ram32(a)); break;
    case 3: if (to_ram) ram32w(a, cd_dma_word()); break;
    case 4: if (to_ram) ram32w(a, spu_dma_read()); else spu_dma_write(ram32(a)); break;
    default: if (to_ram) ram32w(a, 0); break;
    }
}

static void dma_run(int ch) {
    u32 c = dma[ch].chcr;
    const int to_ram = !(c & 1), step = c & 2 ? -4 : 4, sync = (c >> 9) & 3;
    u32 a = dma[ch].madr & 0x1ffffc;
    if (ch == 6) {      /* clears an ordering table, back to front */
        u32 n = dma[ch].bcr & 0xffff;
        if (!n) n = 0x10000;
        for (u32 i = 0; i < n; i++, a -= 4) ram32w(a, i == n - 1 ? 0xffffff : (a - 4) & 0x1fffff);
    } else if (sync == 2) {     /* a linked list of GPU packets */
        for (int guard = 0; guard < 0x40000; guard++) {
            const u32 head = ram32(a), n = head >> 24;
            for (u32 i = 1; i <= n; i++) gpu_gp0(ram32(a + i * 4));
            if (head & 0x800000) break;
            a = head & 0x1ffffc;
        }
        dma[ch].madr = 0xffffff;
    } else {
        u32 n = sync == 0 ? (dma[ch].bcr & 0xffff ? dma[ch].bcr & 0xffff : 0x10000)
                          : (dma[ch].bcr & 0xffff) * (dma[ch].bcr >> 16);
        for (u32 i = 0; i < n; i++, a = (a + step) & 0x1ffffc) dma_word(ch, to_ram, a);
        if (sync == 1) { dma[ch].madr = a; dma[ch].bcr &= 0xffff; }
    }
    dma[ch].chcr = c & ~((1u << 24) | (1u << 28));
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dicr_update();
}

static void dma_write(u32 off, u32 v) {
    if (off == 0x70) { dpcr = v; return; }
    if (off == 0x74) {
        dicr = (dicr & 0xff000000 & ~(v & 0x7f000000)) | (v & 0x00ff803f);
        dicr_update();
        return;
    }
    const int ch = off >> 4;
    if (ch > 6) return;
    switch (off & 15) {
    case 0: dma[ch].madr = v & 0xffffff; break;
    case 4: dma[ch].bcr = v; break;
    case 8:
        dma[ch].chcr = ch == 6 ? (v & 0x51000000) | 2 : v;
        if ((dpcr >> (ch * 4 + 3)) & 1) {
            const int sync = (dma[ch].chcr >> 9) & 3;
            if (ch == 1 && !mdec_has_output()) break;      /* runs when the MDEC has pixels */
            if ((dma[ch].chcr & (1u << 24)) && (sync != 0 || (dma[ch].chcr & (1u << 28)))) dma_run(ch);
        }
        break;
    }
}

void dma_mdec_ready(void) {
    if ((dma[1].chcr & (1u << 24)) && ((dpcr >> 7) & 1)) dma_run(1);
}

static u32 dma_read(u32 off) {
    if (off == 0x70) return dpcr;
    if (off == 0x74) return dicr;
    const int ch = off >> 4;
    if (ch > 6) return 0;
    switch (off & 15) {
    case 0: return dma[ch].madr;
    case 4: return dma[ch].bcr;
    case 8: return dma[ch].chcr;
    }
    return 0;
}

/* ---- the pad port ---- */

static u32 joy_ctrl, joy_mode, joy_baud, joy_stat = 5;
static u8 joy_rx;
static int joy_state, joy_ack_pending, joy_device;

void pad_event(void) {
    if (joy_ack_pending) {
        joy_ack_pending = 0;
        joy_stat |= 0x80 | 0x200;
        if (joy_ctrl & 0x1000) irq_raise(7);
    }
}

static void joy_send(u8 tx) {
    u8 rx = 0xff;
    int ack = 0;
    if (!(joy_ctrl & 2)) { joy_rx = 0xff; joy_stat |= 2; return; }
    const int port = (joy_ctrl >> 13) & 1;
    if (joy_state == 0) {
        joy_device = port == 0 && tx == 0x01 ? 1 : 0;      /* pad 1; no memory cards yet */
        if (joy_device) { ack = 1; joy_state = 1; }
    } else if (joy_device == 1) {
        const u16 b = ~pad_buttons;
        switch (joy_state) {
        case 1: rx = 0x41; ack = 1; break;
        case 2: rx = 0x5a; ack = 1; break;
        case 3: rx = b & 0xff; ack = 1; break;
        case 4: rx = b >> 8; ack = 0; break;
        }
        joy_state = ack ? joy_state + 1 : 5;
    }
    joy_rx = rx;
    joy_stat |= 2;
    if (ack) { joy_ack_pending = 1; event_at(EV_PAD, ps1_now + 1500); }
}

u32 sio_read(int reg, int size) {
    if (reg >= 0x10) return 0;      /* the serial port: nothing on it */
    switch (reg) {
    case 0: { const u32 v = joy_rx; joy_stat &= ~2u; (void)size; return v | (u32)joy_rx << 8 | (u32)joy_rx << 16 | (u32)joy_rx << 24; }
    case 4: { const u32 v = joy_stat | 5; joy_stat &= ~0x80u; return v; }
    case 8: return joy_mode;
    case 10: return joy_ctrl;
    case 14: return joy_baud;
    }
    return 0;
}

void sio_write(int reg, u32 v, int size) {
    (void)size;
    if (reg >= 0x10) return;
    switch (reg) {
    case 0: joy_send(v); break;
    case 8: joy_mode = v & 0xffff; break;
    case 10:
        joy_ctrl = v & 0xffff;
        if (v & 0x40) { joy_ctrl = 0; joy_stat = 5; joy_state = 0; }
        if (v & 0x10) { joy_stat &= ~0x200u; }
        if (!(v & 2)) joy_state = 0;
        break;
    case 14: joy_baud = v & 0xffff; break;
    }
}

/* ---- the bus ---- */

u32 bus_read(u32 addr, int size) {
    const u32 a = addr & 0x1fffffff;
    if (addr == 0xfffe0130) return cache_ctl;
    if (a < 0x800000) { u32 v = 0; memcpy(&v, ps1_ram + (a & 0x1fffff), size); return v; }
    if (a >= 0x1fc00000 && a < 0x1fc80000) { u32 v = 0; memcpy(&v, ps1_bios + (a & 0x7ffff), size); return v; }
    if (a >= 0x1f800000 && a < 0x1f800400) { u32 v = 0; memcpy(&v, ps1_scratch + (a & 0x3ff), size); return v; }
    if (a >= 0x1f000000 && a < 0x1f800000) return size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
    if (a >= 0x1f801000 && a < 0x1f803000) {
        const u32 o = a - 0x1f801000;
        if (o < 0x24) return memctl[o / 4];
        if (o >= 0x40 && o < 0x60) return sio_read(o - 0x40, size);
        if (o == 0x60) return ram_size;
        if (o == 0x70) return i_stat;
        if (o == 0x74) return i_mask;
        if (o >= 0x80 && o < 0x100) return dma_read(o - 0x80);
        if (o >= 0x100 && o < 0x130) return timer_read((o - 0x100) >> 4, (o & 15) >> 2);
        if (o >= 0x800 && o < 0x804) {
            if (size == 1) return cd_read(o - 0x800);
            u32 v = 0;
            for (int i = 0; i < size; i++) v |= (u32)cd_read(2) << (8 * i);     /* wider reads take from the data FIFO */
            return v;
        }
        if (o == 0x810) return gpu_read();
        if (o == 0x814) return gpu_stat();
        if (o == 0x820 || o == 0x824) return mdec_read((o - 0x820) / 4);
        if (o >= 0xc00 && o < 0x1000) {
            if (size == 4) return spu_read(a) | (u32)spu_read(a + 2) << 16;
            return spu_read(a & ~1u);
        }
        return 0;
    }
    return 0;
}

void bus_write(u32 addr, u32 v, int size) {
    const u32 a = addr & 0x1fffffff;
    if (addr == 0xfffe0130) { cache_ctl = v; return; }
    if (a < 0x800000) { memcpy(ps1_ram + (a & 0x1fffff), &v, size); return; }
    if (a >= 0x1f800000 && a < 0x1f800400) { memcpy(ps1_scratch + (a & 0x3ff), &v, size); return; }
    if (a >= 0x1f801000 && a < 0x1f803000) {
        const u32 o = a - 0x1f801000;
        if (o < 0x24) { memctl[o / 4] = v; return; }
        if (o >= 0x40 && o < 0x60) { sio_write(o - 0x40, v, size); return; }
        if (o == 0x60) { ram_size = v; return; }
        if (o == 0x70) { i_stat &= v; irq_update(); return; }
        if (o == 0x74) { i_mask = v & 0x7ff; irq_update(); return; }
        if (o >= 0x80 && o < 0x100) { dma_write(o - 0x80, v); return; }
        if (o >= 0x100 && o < 0x130) { timer_write((o - 0x100) >> 4, (o & 15) >> 2, v); return; }
        if (o >= 0x800 && o < 0x804) { cd_write(o - 0x800, v); return; }
        if (o == 0x810) { gpu_gp0(v); return; }
        if (o == 0x814) { gpu_gp1(v); return; }
        if (o == 0x820 || o == 0x824) { mdec_write((o - 0x820) / 4, v); return; }
        if (o >= 0xc00 && o < 0x1000) {
            spu_write(a, v & 0xffff);
            if (size == 4) spu_write(a + 2, v >> 16);
            return;
        }
        return;     /* 0x1f802000: the expansion's POST display */
    }
}

void bus_reset(void) {
    for (int i = 0; i < EV_COUNT; i++) events[i] = ~0ull;
    event_at(EV_LINE, LINE_CYCLES);
    for (int n = 0; n < 3; n++) tm[n].mode = 1u << 10;
}

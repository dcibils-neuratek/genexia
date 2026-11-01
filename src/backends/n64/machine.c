/*
 * machine.c - the Nintendo 64: physical memory map, MI/SP/DP/VI/AI/PI/SI
 * registers, DMA, the joybus controller protocol, EEPROM, a direct boot that
 * does what the IPL/CIC-6102 would do, and the frame scheduler. The CPU, RSP
 * and RDP live in src/chips behind the interface in n64.h.
 */
#include <stdlib.h>
#include <string.h>
#include "r4300_recomp.h"
#include "n64_machine.h"
#include "m68k.h"              /* rom[] / rom_size */
#include "bus.h"               /* pad_state */

N64_STATE u8 n64_rdram[N64_RDRAM_SIZE];
N64_STATE u8 n64_sp_mem[0x2000];

#define R32(p)    ((u32)(p)[0] << 24 | (u32)(p)[1] << 16 | (u32)(p)[2] << 8 | (p)[3])
#define R16(p)    ((u16)((p)[0] << 8 | (p)[1]))
#define W16(p, v) ((p)[0] = (u8)((v) >> 8), (p)[1] = (u8)(v))
#define W32(p, v) ((p)[0] = (u8)((v) >> 24), (p)[1] = (u8)((v) >> 16), (p)[2] = (u8)((v) >> 8), (p)[3] = (u8)(v))

enum { MI_SP = 1, MI_SI = 2, MI_AI = 4, MI_VI = 8, MI_PI = 16, MI_DP = 32 };
enum { SP_HALT = 1, SP_BROKE = 2, SP_INTR_BREAK = 0x40 };

N64_STATE static u32 mi_intr, mi_mask, mi_mode;
N64_STATE static u32 sp_reg[8], sp_status = SP_HALT, sp_sema;
N64_STATE static u32 dpc_start, dpc_end, dpc_cur, dpc_status;
N64_STATE static u32 vi_reg[14];
N64_STATE static u32 pi_reg[13];
N64_STATE static u32 si_dram, si_status;
N64_STATE static u32 ai_addr[2], ai_len[2], ai_pending_count, ai_dacrate, ai_control;
N64_STATE static u32 ri_reg[8];
N64_STATE static u8  pif_ram[64];
N64_STATE static u8  eeprom[512];
N64_STATE static int cur_line;
N64_STATE static s64 ai_frac;

/* development counters */
#include <stdio.h>
static long st_dp_bytes;
static long st_sp_start, st_dp_lists, st_pi_dma, st_pi_bytes, st_vi, st_si, st_ai_buf;
void n64_debug_print(void) {
    r4300_recomp_print();
    fprintf(stderr, "dp bytes in logged window %ld\n", st_dp_bytes);
    fprintf(stderr, "N64: rsp starts %ld, rdp lists %ld, pi dma %ld (%ld KB), vi irq %ld, si %ld, ai bufs %ld, "
            "vi origin %06X w %u type %u, mi_mask %02X, sp_status %03X\n",
            st_sp_start, st_dp_lists, st_pi_dma, st_pi_bytes / 1024, st_vi, st_si, st_ai_buf,
            vi_reg[1], vi_reg[2], vi_reg[0] & 3, mi_mask, sp_status);
}
static inline void mi_update(void) { r4300_set_irq((mi_intr & mi_mask) != 0); }
static inline void mi_raise(u32 b) { mi_intr |= b; mi_update(); }
static inline void mi_clear(u32 b) { mi_intr &= ~b; mi_update(); }

/* ------------------------------------------------------------ SP DMA */
static void sp_dma(u32 lenreg, int to_rdram) {
    u32 len = (lenreg & 0xFFF) + 1, count = ((lenreg >> 12) & 0xFF) + 1, skip = lenreg >> 20;
    u32 mem = sp_reg[0] & 0x1FFF, dram = sp_reg[1] & 0xFFFFFF;
    u32 bank = mem & 0x1000;
    len = (len + 7) & ~7u;
    for (u32 c = 0; c < count; c++) {
        for (u32 i = 0; i < len; i++) {
            u8 *s = &n64_sp_mem[bank | ((mem + i) & 0xFFF)];
            u32 d = (dram + i) & (N64_RDRAM_SIZE - 1);
            if (to_rdram) n64_rdram[d] = *s; else *s = n64_rdram[d];
        }
        mem += len; dram += len + skip;
    }
    sp_reg[0] = bank | (mem & 0xFFF);
    sp_reg[1] = dram & 0xFFFFFF;
    sp_reg[to_rdram ? 3 : 2] = 0xFF8u | (skip << 20);
}

/* Enhancement: wide 3D. Every frame the display list is read before the RSP
 * gets it, and perspective projections are narrowed in x. That widens the view;
 * the replay in rdp.c then stretches the squeezed picture over the wide image.
 * Game logic doesn't read these matrices back, so nothing else notices.
 * A matrix may be used twice or live for many frames: wide_seen stops it being
 * scaled again. */
static s64 wide_k;                 /* 16.16 factor on x, 0 = off */
static int wide_3d;
static struct { u32 pa, value; } wide_seen[32];
static int wide_next;

/* Super Mario 64 (USA) skips objects outside the original view: its two side
 * checks become plain branches. */
static const struct { u32 pa, normal, wide; } wide_code[] = {
    { 0x27D6A0, 0x45000003, 0x10000003 },
    { 0x27D6D4, 0x45000003, 0x10000003 },
};

void n64_set_wide(double aspect) {
    wide_k = aspect > 4.0 / 3.0 ? (s64)(65536.0 * (4.0 / 3.0) / aspect) : 0;
}

static s32 mtx_get(u32 pa, int i) {
    return (s32)((u32)R16(&n64_rdram[pa + (u32)i * 2]) << 16 | R16(&n64_rdram[pa + 32 + (u32)i * 2]));
}
static void mtx_put(u32 pa, int i, s32 v) {
    W16(&n64_rdram[pa + (u32)i * 2], (u16)((u32)v >> 16));
    W16(&n64_rdram[pa + 32 + (u32)i * 2], (u16)v);
}
static void wide_matrix(u32 pa) {
    if (pa + 64 > N64_RDRAM_SIZE) return;
    /* the perspective signature; orthographic HUD matrices don't have it */
    if (mtx_get(pa, 11) >= 0 || mtx_get(pa, 15) != 0 || mtx_get(pa, 3) != 0 || mtx_get(pa, 7) != 0) return;
    wide_3d = 1;
    const s32 m00 = mtx_get(pa, 0);
    for (int i = 0; i < 32; i++)
        if (wide_seen[i].pa == pa && (s32)wide_seen[i].value == m00) return;
    for (int i = 0; i < 16; i += 4) mtx_put(pa, i, (s32)(((s64)mtx_get(pa, i) * wide_k) >> 16));
    wide_seen[wide_next].pa = pa;
    wide_seen[wide_next].value = (u32)mtx_get(pa, 0);
    wide_next = (wide_next + 1) & 31;
}

/* walk a Fast3D display list the way the microcode would */
int n64_dl_log;
static u32 seg_base[16];
static void dl_walk(u32 addr, int depth, long *c) {
    for (int i = 0; i < 200000; i++) {
        u32 pa = (seg_base[(addr >> 24) & 15] + (addr & 0xFFFFFF)) & 0x7FFFFF;
        const u8 *p = &n64_rdram[pa];
        u32 w0 = R32(p), w1 = R32(p + 4);
        u8 op = (u8)(w0 >> 24);
        addr += 8;
        c[op]++;
        if (op == 0xBC && (w0 & 0xFF) == 6) seg_base[(w0 >> 10) & 15] = w1 & 0xFFFFFF;  /* G_MW_SEGMENT */
        else if (op == 0x06) {            /* G_DL */
            if (depth < 18) dl_walk(w1, depth + 1, c);
            if ((w0 >> 16) & 1) return;
        } else if (op == 0x01) {          /* G_MTX, on the projection stack */
            if (wide_k && ((w0 >> 16) & 1))
                wide_matrix((seg_base[(w1 >> 24) & 15] + (w1 & 0xFFFFFF)) & 0x7FFFFC);
        } else if (op == 0xB8) return;    /* G_ENDDL */
    }
}
static void sp_task_debug(void) {
    u32 type = R32(&n64_sp_mem[0xFC0]);
    if (type != 1) return;
    u32 dl = R32(&n64_sp_mem[0xFC0 + 0x30]);
    static long c[256];
    memset(c, 0, sizeof c);
    dl_walk(dl, 0, c);
    fprintf(stderr, "F3D task: dlist %06X  VTX %ld  TRI1 %ld  DL %ld  SETTIMG %ld  LOADBLOCK %ld  TEXRECT %ld  ucode %06X\n",
            dl, c[0x04], c[0xBF], c[0x06], c[0xFD], c[0xF3], c[0xE4], R32(&n64_sp_mem[0xFC0 + 0x10]));
}

static void wide_frame(void) {
    static int patched;
    if (R32(&n64_sp_mem[0xFC0]) != 1 || (!wide_k && !patched)) return;
    patched = wide_k != 0;
    for (size_t i = 0; i < sizeof wide_code / sizeof wide_code[0]; i++) {
        const u32 pa = wide_code[i].pa, from = wide_k ? wide_code[i].normal : wide_code[i].wide;
        if (R32(&n64_rdram[pa]) != from) continue;
        W32(&n64_rdram[pa], wide_k ? wide_code[i].wide : wide_code[i].normal);
        r4300_code_written(pa, 4);
    }
    static long c[256];
    wide_3d = 0;
    if (wide_k) dl_walk(R32(&n64_sp_mem[0xFC0 + 0x30]), 0, c);
    rdp_wide_frame(wide_3d);
}

#ifdef N64_BENCH
/* For timing the game alone: RSP tasks take no time and do nothing. */
static int bench_task;
static void bench_finish(void) {
    sp_status |= SP_HALT | SP_BROKE | 0x200;      /* signal 2: task done */
    if (sp_status & SP_INTR_BREAK) mi_raise(MI_SP);
    if (R32(&n64_sp_mem[0xFC0]) == 1) mi_raise(MI_DP);
}
#endif

static void sp_status_write(u32 v) {
    if ((v & 1) && n64_dl_log) sp_task_debug();
#ifndef N64_BENCH
    if (v & 1) wide_frame();
#endif
    if (v & 1) { sp_status &= ~(u32)SP_HALT; st_sp_start++; }
#ifdef N64_BENCH
    if (v & 1) bench_task = 800;
#endif
    if (v & 2) sp_status |= SP_HALT;
    if (v & 4) sp_status &= ~(u32)SP_BROKE;
    if (v & 8) mi_clear(MI_SP);
    if (v & 16) mi_raise(MI_SP);
    if (v & 0x80) sp_status &= ~(u32)SP_INTR_BREAK;
    if (v & 0x100) sp_status |= SP_INTR_BREAK;
    for (int i = 0; i < 8; i++) {          /* signals: clear bit 9+2i, set bit 10+2i */
        if (v & (1u << (9 + 2 * i))) sp_status &= ~(1u << (7 + i));
        if (v & (1u << (10 + 2 * i))) sp_status |= 1u << (7 + i);
    }
}
static u32 sp_reg_read(int r) {
    switch (r) {
    case 4: return sp_status;
    case 5: case 6: return 0;
    case 7: { u32 v = sp_sema; sp_sema = 1; return v; }
    default: return sp_reg[r];
    }
}
static void sp_reg_write(int r, u32 v) {
    switch (r) {
    case 0: case 1: sp_reg[r] = v; break;
    case 2: sp_dma(v, 0); break;
    case 3: sp_dma(v, 1); break;
    case 4: sp_status_write(v); break;
    case 7: sp_sema = 0; break;
    default: break;
    }
}

/* ---------------------------------------------------------------- DP */
static u32 dp_reg_read(int r) {
    switch (r) {
    case 0: return dpc_start;
    case 1: return dpc_end;
    case 2: return dpc_cur;
    case 3: return dpc_status | 0x80;   /* cbuf ready */
    default: return 0;
    }
}
static void dp_reg_write(int r, u32 v) {
    switch (r) {
    case 0: dpc_start = v & 0xFFFFF8; dpc_cur = dpc_start; break;
    case 1:
        dpc_end = v & 0xFFFFF8;
        st_dp_lists++;
        if (n64_dl_log > 1 && n64_dl_log < 40) { n64_dl_log++; fprintf(stderr, "DPC end %06X cur %06X start %06X status %X\n", dpc_end, dpc_cur, dpc_start, dpc_status); }
        if (n64_dl_log) st_dp_bytes += (long)dpc_end - (long)dpc_cur;
        if (dpc_end > dpc_cur || (dpc_status & 1)) rdp_run(dpc_cur, dpc_end, dpc_status & 1);
        dpc_cur = dpc_end;
        break;
    case 3:
        if (v & 1) dpc_status &= ~1u;
        if (v & 2) dpc_status |= 1;
        break;
    default: break;
    }
}
void n64_rdp_full_sync(void) { mi_raise(MI_DP); }

u32  n64_rsp_cop0_read(int r) { return r < 8 ? sp_reg_read(r) : dp_reg_read(r - 8); }
void n64_rsp_cop0_write(int r, u32 v) { if (r < 8) sp_reg_write(r, v); else dp_reg_write(r - 8, v); }
void n64_sp_break(void) {
    sp_status |= SP_BROKE | SP_HALT;
    if (sp_status & SP_INTR_BREAK) mi_raise(MI_SP);
}
int n64_sp_halted(void) { return (sp_status & (SP_HALT | SP_BROKE)) != 0; }

/* ---------------------------------------------------------------- PI */
void (*n64_register_game)(void);   /* generated: recompiled code for this ROM */
int n64_force_interp;
__attribute__((weak)) void recomp_register_sm64(void) {}   /* until recomps/n64/sm64 is generated */
static FILE *cov_log;
static u32 *rom_of;                /* SF2_MISSLOG: ROM offset of each RDRAM word */

static void pi_dma_to_rdram(u32 len) {
    u32 d = pi_reg[0] & 0xFFFFFF, c = pi_reg[1] & 0x0FFFFFFF;
    u32 n = (len & 0xFFFFFF) + 1;
    for (u32 i = 0; i < n; i++) {
        u32 src = c + i;
        n64_rdram[(d + i) & (N64_RDRAM_SIZE - 1)] = src < rom_size ? rom[src] : 0;
    }
    pi_reg[0] = (d + n) & 0xFFFFFF;
    pi_reg[1] = (pi_reg[1] + n);
    r4300_code_written(d, n);
    if (rom_of) {                  /* dev: remember where code comes from */
        fprintf(cov_log, "D %06X %07X %X\n", d, c, n); fflush(cov_log);
        for (u32 i = 0; i + 4 <= n; i += 4) rom_of[((d + i) & (N64_RDRAM_SIZE - 1)) >> 2] = c + i;
    }
    st_pi_dma++; st_pi_bytes += n;
    mi_raise(MI_PI);
}

/* ------------------------------------------------- SI, PIF, controllers */
u8 n64_stick_x[2], n64_stick_y[2];  /* set by the front end (signed values) */
u8 n64_c_buttons[2];                /* 1 up, 2 down, 4 left, 8 right */

static u16 pad_buttons(int p) {
    u16 g = pad_state[p], v = 0;
    if (g & PAD_A) v |= 0x8000;                  /* A */
    if (g & (PAD_B | PAD_X)) v |= 0x4000;        /* B */
    if (g & PAD_C) v |= 0x2000;                  /* Z */
    if (g & PAD_START) v |= 0x1000;
    if (g & PAD_Z) v |= 0x0010;                  /* R */
    if (g & PAD_MODE) v |= 0x0020;               /* L */
    u8 c = n64_c_buttons[p];
    if (g & PAD_Y) c |= 1;
    if (c & 1) v |= 0x0008;
    if (c & 2) v |= 0x0004;
    if (c & 4) v |= 0x0002;
    if (c & 8) v |= 0x0001;
    return v;
}
static void joybus_controller(int ch, const u8 *tx, u8 *rx, int rxn, u8 *status) {
    switch (tx[0]) {
    case 0x00: case 0xFF:
        if (ch < 2) {          /* standard controller, no pak */
            rx[0] = 0x05; rx[1] = 0x00; rx[2] = 0x02;
        } else if (ch == 4) {  /* 4 Kbit EEPROM */
            rx[0] = 0x00; rx[1] = 0x80; rx[2] = 0x00;
        } else *status |= 0x80;
        break;
    case 0x01:
        if (ch < 2 && rxn >= 4) {
            u16 b = pad_buttons(ch);
            s8 x = (s8)n64_stick_x[ch], y = (s8)n64_stick_y[ch];
            u16 g = pad_state[ch];
            if (!x) x = (g & PAD_LEFT) ? -80 : (g & PAD_RIGHT) ? 80 : 0;
            if (!y) y = (g & PAD_DOWN) ? -80 : (g & PAD_UP) ? 80 : 0;
            rx[0] = (u8)(b >> 8); rx[1] = (u8)b; rx[2] = (u8)x; rx[3] = (u8)y;
        } else *status |= 0x80;
        break;
    case 0x04:                  /* EEPROM read */
        if (ch == 4) memcpy(rx, eeprom + ((u32)tx[1] * 8 & 0x1FF), 8); else *status |= 0x80;
        break;
    case 0x05:                  /* EEPROM write */
        if (ch == 4) { memcpy(eeprom + ((u32)tx[1] * 8 & 0x1FF), tx + 2, 8); rx[0] = 0; } else *status |= 0x80;
        break;
    default: *status |= 0x80; break;
    }
}
static void pif_process(void) {
    int ch = 0;
    for (int i = 0; i < 63 && ch < 6; ) {
        u8 t = pif_ram[i];
        if (t == 0xFE) break;
        if (t == 0xFF) { i++; continue; }      /* pad byte */
        if (t == 0x00) { ch++; i++; continue; }/* skip channel */
        if (t & 0x80) { i++; continue; }
        int txn = t & 0x3F;
        u8 r = pif_ram[i + 1];
        int rxn = r & 0x3F;
        if (i + 2 + txn + rxn > 64) break;
        u8 *txp = &pif_ram[i + 2], *rxp = txp + txn;
        joybus_controller(ch, txp, rxp, rxn, &pif_ram[i + 1]);
        i += 2 + txn + rxn;
        ch++;
    }
}
static void si_to_rdram(void) {
    st_si++;
    pif_process();
    memcpy(&n64_rdram[si_dram & 0x7FFFFC], pif_ram, 64);
    si_status |= 0x1000;
    mi_raise(MI_SI);
}
static void si_from_rdram(void) {
    memcpy(pif_ram, &n64_rdram[si_dram & 0x7FFFFC], 64);
    si_status |= 0x1000;
    mi_raise(MI_SI);
}

/* ---------------------------------------------------------------- AI */
#define AUD_RING 16384
static s16 aud_ring[AUD_RING * 2];
N64_STATE static u32 aud_w, aud_r;
int n64_audio_rate = 32000;

static void ai_step(int cycles) {
    if (!ai_pending_count || !(ai_control & 1)) return;
    if (!ai_dacrate) return;
    double hz = 48681812.0 / (ai_dacrate + 1);
    n64_audio_rate = (int)hz;
    ai_frac += (s64)(cycles * hz * 65536.0 / 93750000.0);
    while (ai_frac >= 65536 && ai_pending_count) {
        ai_frac -= 65536;
        u8 *p = &n64_rdram[ai_addr[0] & 0x7FFFFC];
        s16 l = (s16)(p[0] << 8 | p[1]), r = (s16)(p[2] << 8 | p[3]);
        aud_ring[(aud_w % AUD_RING) * 2] = l;
        aud_ring[(aud_w % AUD_RING) * 2 + 1] = r;
        aud_w++;
        ai_addr[0] += 4;
        if (ai_len[0] > 4) ai_len[0] -= 4;
        else {
            ai_len[0] = 0;
            ai_addr[0] = ai_addr[1]; ai_len[0] = ai_len[1]; ai_len[1] = 0;
            ai_pending_count--;
            st_ai_buf++;
            mi_raise(MI_AI);
        }
    }
}
int n64_audio_take(s16 *out, int max) {
    int n = 0;
    if (aud_w - aud_r > AUD_RING) aud_r = aud_w - AUD_RING;
    while (aud_r != aud_w && n < max) {
        out[n * 2] = aud_ring[(aud_r % AUD_RING) * 2];
        out[n * 2 + 1] = aud_ring[(aud_r % AUD_RING) * 2 + 1];
        aud_r++; n++;
    }
    return n;
}
static void ai_write(int r, u32 v) {
    switch (r) {
    case 0: ai_addr[ai_pending_count ? 1 : 0] = v & 0xFFFFF8; break;
    case 1:
        v &= 0x3FFF8;
        if (v && ai_pending_count < 2) { ai_len[ai_pending_count] = v; ai_pending_count++; }
        break;
    case 2: ai_control = v; break;
    case 3: mi_clear(MI_AI); break;
    case 4: ai_dacrate = v & 0x3FFF; break;
    default: break;
    }
}
static u32 ai_read(int r) {
    if (r == 1) return ai_len[0];
    if (r == 3) return (ai_pending_count >= 2 ? 0x80000001u : 0) | (ai_pending_count ? 0x40000000u : 0);
    return 0;
}

/* --------------------------------------------------------- memory map */
static u32 io_read(u32 pa) {
    u32 b = pa & 0xFFF00000, r = (pa >> 2) & 0xF;
    if (pa < 0x04000000) return 0;
    if (pa < 0x04002000) return R32(&n64_sp_mem[pa & 0x1FFC]);
    if (pa >= 0x04040000 && pa < 0x04040020) return sp_reg_read((pa >> 2) & 7);
    if (pa == 0x04080000) return rsp_get_pc();
    switch (b) {
    case 0x04100000: return dp_reg_read((pa >> 2) & 7);
    case 0x04300000:
        switch (r) { case 0: return mi_mode; case 1: return 0x02020102; case 2: return mi_intr; case 3: return mi_mask; default: return 0; }
    case 0x04400000: return r == 4 ? (u32)(cur_line * 2) : vi_reg[r > 13 ? 13 : r];
    case 0x04500000: return ai_read((pa >> 2) & 7);
    case 0x04600000: return r == 4 ? 0 : pi_reg[r > 12 ? 12 : r];
    case 0x04700000: return ri_reg[(pa >> 2) & 7];
    case 0x04800000: return r == 6 ? si_status : 0;
    default: break;
    }
    if (pa >= 0x1FC007C0 && pa < 0x1FC00800) return R32(&pif_ram[pa & 0x3C]);
    if (pa >= 0x10000000 && pa < 0x1FC00000) {
        u32 o = pa & 0x0FFFFFFC;
        return o + 3 < rom_size ? R32(&rom[o]) : 0;
    }
    return 0;
}
static void io_write(u32 pa, u32 v) {
    u32 b = pa & 0xFFF00000, r = (pa >> 2) & 0xF;
    if (pa < 0x04000000) return;
    if (pa < 0x04002000) { W32(&n64_sp_mem[pa & 0x1FFC], v); return; }
    if (pa >= 0x04040000 && pa < 0x04040020) { sp_reg_write((pa >> 2) & 7, v); return; }
    if (pa == 0x04080000) { rsp_set_pc(v & 0xFFC); return; }
    switch (b) {
    case 0x04100000: dp_reg_write((pa >> 2) & 7, v); return;
    case 0x04300000:
        if (r == 0) {
            mi_mode = v & 0x7F;
            if (v & 0x800) mi_clear(MI_DP);
        } else if (r == 3) {
            for (int i = 0; i < 6; i++) {
                if (v & (1u << (i * 2))) mi_mask &= ~(1u << i);
                if (v & (2u << (i * 2))) mi_mask |= 1u << i;
            }
            mi_update();
        }
        return;
    case 0x04400000:
        if (r == 4) mi_clear(MI_VI); else if (r < 14) vi_reg[r] = v;
        return;
    case 0x04500000: ai_write((pa >> 2) & 7, v); return;
    case 0x04600000:
        if (r == 4) { if (v & 2) mi_clear(MI_PI); return; }
        if (r == 3) { pi_dma_to_rdram(v); return; }
        if (r == 2) { mi_raise(MI_PI); return; }
        if (r < 13) pi_reg[r] = v;
        return;
    case 0x04700000: ri_reg[(pa >> 2) & 7] = v; return;
    case 0x04800000:
        if (r == 0) si_dram = v;
        else if (r == 1) si_to_rdram();
        else if (r == 4) si_from_rdram();
        else if (r == 6) { si_status &= ~0x1000u; mi_clear(MI_SI); }
        return;
    default: break;
    }
    if (pa >= 0x1FC007C0 && pa < 0x1FC00800) {
        W32(&pif_ram[pa & 0x3C], v);
        if (pa >= 0x1FC007FC) { pif_process(); mi_raise(MI_SI); }
    }
}

u32 n64_read32(u32 pa) {
    if (pa < N64_RDRAM_SIZE) return R32(&n64_rdram[pa & ~3u]);
    return io_read(pa & ~3u);
}
u16 n64_read16(u32 pa) { return (u16)(n64_read32(pa & ~3u) >> (16 - (pa & 2) * 8)); }
u8  n64_read8(u32 pa) { return (u8)(n64_read32(pa & ~3u) >> (24 - (pa & 3) * 8)); }
u64 n64_read64(u32 pa) { return (u64)n64_read32(pa) << 32 | n64_read32(pa + 4); }
void n64_write32(u32 pa, u32 v) {
    if (pa < N64_RDRAM_SIZE) { u8 *p = &n64_rdram[pa & ~3u]; W32(p, v); return; }
    io_write(pa & ~3u, v);
}
void n64_write64(u32 pa, u64 v) { n64_write32(pa, (u32)(v >> 32)); n64_write32(pa + 4, (u32)v); }
void n64_write16(u32 pa, u16 v) {
    if (pa < N64_RDRAM_SIZE) { n64_rdram[pa] = (u8)(v >> 8); n64_rdram[pa + 1] = (u8)v; return; }
    if (pa >= 0x04000000 && pa < 0x04002000) { u8 *p = &n64_sp_mem[pa & 0x1FFE]; p[0] = (u8)(v >> 8); p[1] = (u8)v; return; }
    io_write(pa & ~3u, (u32)v << (16 - (pa & 2) * 8));
}
void n64_write8(u32 pa, u8 v) {
    if (pa < N64_RDRAM_SIZE) { n64_rdram[pa] = v; return; }
    if (pa >= 0x04000000 && pa < 0x04002000) { n64_sp_mem[pa & 0x1FFF] = v; return; }
    io_write(pa & ~3u, (u32)v << (24 - (pa & 3) * 8));
}

/* -------------------------------------------------------- video output */
static void vi_output(void) {
    if (vdp_skip_render) return;
    u32 origin = vi_reg[1] & 0xFFFFFF, width = vi_reg[2] & 0xFFF, type = vi_reg[0] & 3;
    u32 xs = vi_reg[12] & 0xFFF, ys = vi_reg[13] & 0xFFF;
    if (!width || type < 2 || !origin || !(vi_reg[9] & 0x3FF03FF) || !(vi_reg[10] & 0x3FF03FF)) {   /* osViBlack zeroes H/V_VIDEO */
        memset(vdp_framebuffer, 0, sizeof vdp_framebuffer);
        vdp_screen_w = 320; vdp_screen_h = 240;
        return;
    }
    u32 w = xs ? (width * 1024 + 0) / 1024 : width;
    if (xs) w = xs * 640 / 1024; else w = width;
    if (w > width) w = width;
    if (w > VDP_FB_STRIDE) w = VDP_FB_STRIDE;
    u32 h = ys ? (ys * 240 / 1024) : 240;
    if (h > 240) h = 240;
    if (h < 200) h = 240;
    vdp_screen_w = (int)w; vdp_screen_h = (int)h; vdp_native_w = (int)w;
    for (u32 y = 0; y < h; y++) {
        u32 *out = &vdp_framebuffer[y * VDP_FB_STRIDE];
        if (type == 2) {
            const u8 *p = &n64_rdram[(origin + y * width * 2) & 0x7FFFFE];
            for (u32 x = 0; x < w; x++, p += 2) {
                u32 c = (u32)p[0] << 8 | p[1];
                u32 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
                out[x] = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            }
        } else {
            const u8 *p = &n64_rdram[(origin + y * width * 4) & 0x7FFFFC];
            for (u32 x = 0; x < w; x++, p += 4) out[x] = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        }
    }
}

void n64_state_loaded(void) { r4300_recomp_flush(); rdp_hires_resync(); }
u32 n64_vi_origin(void) { return vi_reg[1] & 0xFFFFFF; }

/* ---------------------------------------------------------- scheduler */
#define CYCLES_PER_LINE 5960       /* 93.75 MHz / 60 Hz / 262 lines */
#define SLICE 1200

void n64_run_frame(void) {
    for (int line = 0; line < 262; line++) {
        cur_line = line;
        if ((vi_reg[3] >> 1) == (u32)line && vi_reg[3]) { mi_raise(MI_VI); st_vi++; }
        int rem = CYCLES_PER_LINE;
        while (rem > 0) {
            int used = r4300_run(rem < SLICE ? rem : SLICE);
#ifdef N64_BENCH
            if (bench_task > 0 && (bench_task -= used) <= 0) bench_finish();
#else
            if (!n64_sp_halted()) rsp_run(used * 2 / 3 + 1);
#endif
            ai_step(used);
            rem -= used;
        }
    }
#ifndef N64_BENCH
    vi_output();
#endif
}

/* ---------------------------------------------------------------- boot */
int n64_detect(void) {
    u32 m = R32(rom);
    return m == 0x80371240 || m == 0x37804012 || m == 0x40123780;
}

const char *n64_load(void) {
    static char title[24];
    u32 m = R32(rom);
    if (m == 0x37804012) {                       /* byte-swapped .v64 */
        for (u32 i = 0; i + 1 < rom_size; i += 2) { u8 t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
    } else if (m == 0x40123780) {                /* little-endian .n64 */
        for (u32 i = 0; i + 3 < rom_size; i += 4) {
            u8 a = rom[i], b = rom[i + 1];
            rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
        }
    }
    memcpy(title, rom + 0x20, 20); title[20] = 0;
    for (int i = 19; i >= 0 && (title[i] == ' ' || !title[i]); i--) title[i] = 0;
    return title;
}

void n64_reset(void) {
    memset(n64_rdram, 0, sizeof n64_rdram);
    memset(n64_sp_mem, 0, sizeof n64_sp_mem);
    memset(eeprom, 0xFF, sizeof eeprom);
    mi_intr = mi_mask = 0; sp_status = SP_HALT; sp_sema = 0;
    rsp_reset(); rdp_reset();

    /* what the PIF ROM and IPL3 leave behind for a CIC-6102 game */
    memcpy(n64_sp_mem, rom, 0x1000);
    u32 entry = R32(rom + 8);
    u32 n = rom_size > 0x101000 ? 0x100000 : rom_size - 0x1000;
    memcpy(&n64_rdram[entry & 0x7FFFFF], rom + 0x1000, n);
    u8 *low = n64_rdram;
    W32(low + 0x300, 1);                  /* osTvType: NTSC */
    W32(low + 0x304, 0);                  /* osRomType: cart */
    W32(low + 0x308, 0xB0000000u);        /* osRomBase */
    W32(low + 0x30C, 0);                  /* osResetType: cold */
    W32(low + 0x310, 0x3F);               /* CIC seed */
    W32(low + 0x318, N64_RDRAM_SIZE);     /* osMemSize */

    static int recomp_ready;
    if (!recomp_ready) {
        recomp_ready = 1;
        if (n64_register_game && !n64_force_interp) { n64_register_game(); r4300_recomp_enable(); }
        const char *p = getenv("SF2_MISSLOG");
        if (p && !n64_force_interp) {
            cov_log = fopen(p, "a");
            rom_of = calloc(N64_RDRAM_SIZE >> 2, sizeof *rom_of);
            r4300_recomp_log(cov_log, rom_of);
        }
    }
    if (rom_of) {
        fprintf(cov_log, "D %06X %07X %X\n", entry & 0x7FFFFF, 0x1000, n);
        for (u32 i = 0; i + 4 <= n; i += 4) rom_of[((entry & 0x7FFFFF) + i) >> 2] = 0x1000 + i;
    }
    r4300_reset(entry);
    for (int i = 0; i < 32; i++) r4300_gpr[i] = 0;
    r4300_gpr[19] = 0;                    /* s3: cartridge */
    r4300_gpr[20] = 1;                    /* s4: NTSC */
    r4300_gpr[21] = 0;                    /* s5: cold reset */
    r4300_gpr[22] = 0x3F;                 /* s6: CIC seed */
    r4300_gpr[23] = 0;                    /* s7 */
    r4300_gpr[29] = 0xFFFFFFFFA4001FF0ull;/* sp */
    r4300_gpr[31] = 0xFFFFFFFFA4001550ull;/* ra */
    r4300_cop0[12] = 0x34000000;          /* Status: CU0, CU1, FR */
    r4300_cop0[16] = 0x0006E463;          /* Config */
    r4300_cop0[15] = 0x00000B00;          /* PRId */
    r4300_cop0[1]  = 31;                  /* Random */
}

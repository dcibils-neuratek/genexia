/*
 * test_apu.c - standalone checks for apu.c and dsp1.c.
 * clang -O2 -Wall -Wextra -std=c11 src/snes/test_apu.c src/snes/apu.c src/snes/dsp1.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes.h"

static int failures;
static s64 master;

#define CHECK(cond, ...) do {                                 \
        if (!(cond)) {                                        \
            failures++;                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

/* Advance by n SPC cycles (rounded up so the fixed-point step always passes). */
static void run_smp(int n)
{
    master += (s64)n * 21477272 / 1024000 + 1;
    snes_apu_run_to(master);
}

static int wait_port(int port, u8 v, int limit)
{
    for (int i = 0; i < limit; i += 8) {
        if (snes_apu_read(port) == v) return 1;
        run_smp(8);
    }
    return 0;
}

/* The CPU side of the upload protocol, as every SNES game has it. */
static void ipl_start(void)
{
    snes_apu_reset();
    CHECK(wait_port(0, 0xAA, 400000), "IPL did not post $AA");
    CHECK(snes_apu_read(1) == 0xBB, "IPL did not post $BB");
}

static u8 ipl_kick = 0xCC;

static void ipl_block(u16 addr, const u8 *data, int len)
{
    snes_apu_write(2, (u8)addr);
    snes_apu_write(3, (u8)(addr >> 8));
    snes_apu_write(1, 1);
    snes_apu_write(0, ipl_kick);
    CHECK(wait_port(0, ipl_kick, 4000), "block kick not echoed");
    for (int i = 0; i < len; i++) {
        snes_apu_write(1, data[i]);
        snes_apu_write(0, (u8)i);
        CHECK(wait_port(0, (u8)i, 4000), "byte %d not echoed", i);
    }
    ipl_kick = (u8)(len + 2);
    if (ipl_kick == 0) ipl_kick = 2;
}

static void ipl_jump(u16 addr)
{
    snes_apu_write(2, (u8)addr);
    snes_apu_write(3, (u8)(addr >> 8));
    snes_apu_write(1, 0);
    snes_apu_write(0, ipl_kick);
    CHECK(wait_port(0, ipl_kick, 4000), "jump kick not echoed");
    ipl_kick = 0xCC;
}

static void test_cpu_ops(void)
{
    static const u8 prog[] = {
        0xE8,0x0C, 0x8D,0x22, 0xCF,           /* mov a,#12 mov y,#34 mul ya   */
        0xC4,0xF5, 0xCB,0xF6,                 /* port1 = lo, port2 = hi       */
        0x8D,0x01, 0xE8,0x04, 0xCD,0x05, 0x9E,/* ya = $0104; x = 5; div       */
        0xC4,0xF7,                            /* port3 = quotient             */
        0x8F,0x5A,0xF4,                       /* port0 = $5A: first batch     */
        0x2F,0xFE
    };
    ipl_start();
    ipl_block(0x0200, prog, (int)sizeof prog);
    ipl_jump(0x0200);
    CHECK(wait_port(0, 0x5A, 20000), "program did not finish");
    CHECK(snes_apu_read(1) == (u8)(12 * 34), "MUL low %02X", snes_apu_read(1));
    CHECK(snes_apu_read(2) == 1, "MUL high %02X", snes_apu_read(2));
    CHECK(snes_apu_read(3) == 260 / 5, "DIV %02X", snes_apu_read(3));

    static const u8 prog2[] = {
        0xE8,0x19, 0x60, 0x88,0x28, 0xDF,     /* 19 + 28 = 47 in BCD          */
        0xC4,0xF5,
        0x8F,0x34,0x10, 0x8F,0x12,0x11,       /* word at $10 = $1234          */
        0x3A,0x10, 0xBA,0x10,                 /* incw $10; movw ya,$10        */
        0xCB,0xF6, 0xC4,0xF7,
        0xE8,0x80, 0x0E,0x20,0x00,            /* tset1 !$0020                 */
        0xE4,0x20,                            /* reads back $80               */
        0x68,0x80,
        0xF0,0x02, 0x2F,0xFE,                 /* hang unless it matched       */
        0x8F,0xA5,0xF4,
        0x2F,0xFE
    };
    ipl_start();
    ipl_block(0x0300, prog2, (int)sizeof prog2);
    ipl_jump(0x0300);
    CHECK(wait_port(0, 0xA5, 20000), "second program did not finish");
    CHECK(snes_apu_read(1) == 0x47, "DAA %02X", snes_apu_read(1));
    CHECK(snes_apu_read(2) == 0x12, "INCW hi %02X", snes_apu_read(2));
    CHECK(snes_apu_read(3) == 0x35, "INCW lo %02X", snes_apu_read(3));
}

static void test_timer(void)
{
    static const u8 prog[] = {
        0x8F,0x0A,0xFA,                       /* T0 target 10                 */
        0x8F,0x01,0xF1,                       /* start T0, boot ROM off       */
        0xE4,0xFD, 0xF0,0xFC,                 /* poll the counter             */
        0xC4,0xF5,
        0x8F,0x5A,0xF4,
        0x2F,0xFE
    };
    ipl_start();
    ipl_block(0x0200, prog, (int)sizeof prog);
    ipl_jump(0x0200);

    int cycles = 0;
    while (snes_apu_read(0) != 0x5A && cycles < 100000) {
        run_smp(4);
        cycles += 4;
    }
    /* 10 ticks of the 128-cycle divider; the first tick has a partial phase */
    CHECK(cycles >= 1100 && cycles <= 1400, "timer took %d cycles", cycles);
    CHECK(snes_apu_read(1) == 1, "counter read %d", snes_apu_read(1));
}

/* Boot a program that copies (reg, value) pairs into the DSP and flags port 0.
 * The sample is one looping BRR block: a loud ramp, range 12, filter 0. */
static void dsp_boot(const u8 *regs, int n)
{
    static const u8 brr[9] = { 0xC3, 0x13, 0x57, 0x7D, 0xB9, 0x75, 0x31, 0xDF, 0xB9 };
    static const u8 dir[4] = { 0x00, 0x03, 0x00, 0x03 };
    u8 prog[0x20 + 64];
    /* mov x,#0 / loop: mov a,!tbl+x / mov $F2,a / inc x / mov a,!tbl+x /
     * mov $F3,a / inc x / cmp x,#n / bne loop / port0 = $77 / self */
    const u8 code[] = {
        0xCD,0x00,
        0xF5,0x20,0x02, 0xC4,0xF2, 0x3D,
        0xF5,0x20,0x02, 0xC4,0xF3, 0x3D,
        0xC8,(u8)n, 0xD0,0xF0,
        0x8F,0x77,0xF4, 0x2F,0xFE
    };
    memset(prog, 0, sizeof prog);
    memcpy(prog, code, sizeof code);
    memcpy(prog + 0x20, regs, (size_t)n);

    ipl_start();
    ipl_block(0x0300, brr, 9);
    ipl_block(0x0400, dir, 4);
    ipl_block(0x0200, prog, 0x20 + n);
    ipl_jump(0x0200);
    CHECK(wait_port(0, 0x77, 40000), "register program did not finish");
}

static void test_dsp_output(void)
{
    static const u8 regs[] = {
        0x6C,0x20, 0x5D,0x04, 0x0C,0x7F, 0x1C,0x7F, 0x2C,0x00, 0x3C,0x00,
        0x00,0x7F, 0x01,0x7F, 0x02,0x00, 0x03,0x10, 0x04,0x00,
        0x05,0x8F, 0x06,0xE0, 0x4C,0x01
    };
    dsp_boot(regs, (int)sizeof regs);

    static s16 pcm[8192];
    int total = 0, peak = 0, changes = 0;
    s16 last = 0;
    for (int i = 0; i < 200; i++) {
        run_smp(64 * 40);
        int n = snes_apu_take(pcm, 4096);
        total += n;
        for (int k = 0; k < n * 2; k += 2) {
            int a = abs(pcm[k]);
            if (a > peak) peak = a;
            if (pcm[k] != last) changes++;
            last = pcm[k];
        }
    }
    CHECK(total >= 15000 && total <= 16500, "got %d samples", total);
    CHECK(peak > 1000, "peak only %d", peak);
    CHECK(changes > 1000, "output hardly changes (%d)", changes);

    /* overflow keeps the newest samples and never exceeds the ring */
    run_smp(32 * 6000);
    int n = snes_apu_take(pcm, 4096);
    CHECK(n > 1068 && n <= 4096, "drained %d", n);
    CHECK(snes_apu_take(pcm, 4096) == 0, "ring should be empty");
}

/* Dry path silent, voice sent only to echo: sound must start one buffer late. */
static void test_echo(void)
{
    static const u8 regs[] = {
        0x6C,0x00, 0x5D,0x04, 0x0C,0x00, 0x1C,0x00, 0x2C,0x7F, 0x3C,0x7F,
        0x6D,0x40, 0x7D,0x01, 0x0D,0x40, 0x7F,0x7F, 0x4D,0x01,
        0x00,0x7F, 0x01,0x7F, 0x02,0x00, 0x03,0x10, 0x04,0x00,
        0x05,0x8F, 0x06,0xE0, 0x4C,0x01
    };
    static s16 pcm[8192];
    dsp_boot(regs, (int)sizeof regs);
    (void)snes_apu_take(pcm, 4096);          /* drop what preceded KON */

    run_smp(32 * 3000);
    int n = snes_apu_take(pcm, 4096), first = -1;
    for (int i = 0; i < n && first < 0; i++)
        if (pcm[i * 2] != 0) first = i;
    /* EDL=1 is 2048 bytes, 512 samples, on top of a stale buffer position */
    CHECK(first >= 400 && first <= 1100, "echo onset at sample %d of %d", first, n);
}

/* ------------------------------------------------------------------ DSP-1 */
static void dsp1_word(s16 v)
{
    CHECK(snes_dsp1_read(1) & 0x80, "RQM clear before word write");
    snes_dsp1_write((u8)v);
    CHECK(snes_dsp1_read(1) & 0x10, "DRS not set after low byte");
    snes_dsp1_write((u8)((u16)v >> 8));
}

static s16 dsp1_get(void)
{
    u8 l = snes_dsp1_read(0);
    u8 h = snes_dsp1_read(0);
    return (s16)(l | h << 8);
}

static void dsp1_cmd(u8 c, const s16 *params, int n)
{
    CHECK(snes_dsp1_read(1) & 0x04, "DRC should be set between commands");
    snes_dsp1_write(c);
    for (int i = 0; i < n; i++) dsp1_word(params[i]);
}

static int close_to(int a, int b, int tol) { return abs(a - b) <= tol; }

static void test_dsp1(void)
{
    s16 r0, r1, r2;

    snes_dsp1_reset();
    CHECK((snes_dsp1_read(1) & 0x84) == 0x84, "status after reset");
    CHECK(snes_dsp1_read(0) == 0x80, "idle data register");

    dsp1_cmd(0x00, (s16[]){ 0x4000, 0x4000 }, 2);
    r0 = dsp1_get();
    CHECK(r0 == 0x2000, "multiply %04X", (u16)r0);

    dsp1_cmd(0x00, (s16[]){ -0x4000, 0x2000 }, 2);
    r0 = dsp1_get();
    CHECK(r0 == -0x1000, "signed multiply %d", r0);

    dsp1_cmd(0x20, (s16[]){ 0x4000, 0x4000 }, 2);
    r0 = dsp1_get();
    CHECK(r0 == 0x2001, "alt multiply %04X", (u16)r0);

    dsp1_cmd(0x10, (s16[]){ 0x4000, 0 }, 2);
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(r0 == 0x7FFF && r1 == 1, "inverse special %04X %d", (u16)r0, r1);

    dsp1_cmd(0x10, (s16[]){ 0x6000, 0 }, 2);       /* 1/0.75 = 1.3333 */
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(close_to(r0, 0x5555, 3) && r1 == 1, "inverse %04X %d", (u16)r0, r1);

    dsp1_cmd(0x10, (s16[]){ -0x2000, 2 }, 2);      /* 1/(-0.25*4) = -1 */
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(r0 == -0x4000 && r1 == 1, "inverse neg %d %d", r0, r1);

    dsp1_cmd(0x10, (s16[]){ 0, 0 }, 2);
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(r0 == 0x7FFF && r1 == 0x2F, "inverse of zero");

    dsp1_cmd(0x04, (s16[]){ 0x4000, 0x1000 }, 2);  /* 90 degrees */
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(close_to(r0, 0x1000, 1) && close_to(r1, 0, 1), "triangle %d %d", r0, r1);

    dsp1_cmd(0x28, (s16[]){ 3000, 4000, 0 }, 3);
    r0 = dsp1_get();
    CHECK(close_to(r0, 5000, 2), "distance %d", r0);

    dsp1_cmd(0x08, (s16[]){ 3, 4, 0 }, 3);
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(r0 == 50 && r1 == 0, "radius %d %d", r0, r1);

    dsp1_cmd(0x18, (s16[]){ 0x4000, 0, 0, 0 }, 4);
    r0 = dsp1_get();
    CHECK(r0 == 0x2000, "range %d", r0);

    dsp1_cmd(0x0C, (s16[]){ 0x4000, 1000, 0 }, 3); /* rotate 90 degrees */
    r0 = dsp1_get(); r1 = dsp1_get();
    CHECK(close_to(r0, 0, 2) && close_to(r1, -1000, 2), "rotate %d %d", r0, r1);

    /* attitude with identity angles, then both directions */
    dsp1_cmd(0x01, (s16[]){ 0x7FFF, 0, 0, 0 }, 4);
    dsp1_cmd(0x0D, (s16[]){ 100, 200, 300 }, 3);
    r0 = dsp1_get(); r1 = dsp1_get(); r2 = dsp1_get();
    CHECK(close_to(r0, 50, 2) && close_to(r1, 100, 2) && close_to(r2, 150, 2),
          "object %d %d %d", r0, r1, r2);
    dsp1_cmd(0x03, (s16[]){ 100, 200, 300 }, 3);
    r0 = dsp1_get(); r1 = dsp1_get(); r2 = dsp1_get();
    CHECK(close_to(r0, 50, 2) && close_to(r1, 100, 2) && close_to(r2, 150, 2),
          "global %d %d %d", r0, r1, r2);
    dsp1_cmd(0x0B, (s16[]){ 100, 200, 300 }, 3);
    r0 = dsp1_get();
    CHECK(close_to(r0, 50, 2), "scalar %d", r0);

    dsp1_cmd(0x0F, (s16[]){ 0 }, 1);
    r0 = dsp1_get();
    CHECK(r0 == 0, "memory test");
    dsp1_cmd(0x2F, (s16[]){ 0 }, 1);
    r0 = dsp1_get();
    CHECK(r0 == 0x0100, "version %04X", (u16)r0);

    dsp1_cmd(0x1F, (s16[]){ 0 }, 1);
    int sum = 0;
    for (int i = 0; i < 1024; i++) sum += dsp1_get() != 0;
    CHECK(sum > 100, "ROM dump empty");
    CHECK(snes_dsp1_read(1) & 0x04, "ROM dump did not end");

    /* a Mario Kart style view: projection and ground round trip */
    static const s16 aas[] = { 0, 0x2000, -0x3000, 0x7000 };
    for (unsigned t = 0; t < sizeof aas / sizeof aas[0]; t++) {
        s16 fx = 512, fy = 600, a = aas[t];
        dsp1_cmd(0x02, (s16[]){ fx, fy, 0, 0x0100, 0x0200, a, 0x2800 }, 7);
        s16 vof = dsp1_get(), vva = dsp1_get(), cx = dsp1_get(), cy = dsp1_get();
        (void)vof; (void)vva;

        /* the centre of the screen projects back to Cx,Cy */
        dsp1_cmd(0x06, (s16[]){ cx, cy, 0 }, 3);
        r0 = dsp1_get(); r1 = dsp1_get(); r2 = dsp1_get();
        CHECK(close_to(r0, 0, 4) && close_to(r1, 0, 4),
              "project centre %d %d (m=%d)", r0, r1, r2);

        /* screen -> ground -> screen */
        static const s16 hv[][2] = { {40, 30}, {-60, 50}, {80, -20}, {-20, -30} };
        for (int k = 0; k < 4; k++) {
            dsp1_cmd(0x0E, (s16[]){ hv[k][0], hv[k][1] }, 2);
            r0 = dsp1_get(); r1 = dsp1_get();
            s16 tx = r0, ty = r1;
            dsp1_cmd(0x06, (s16[]){ tx, ty, 0 }, 3);
            r0 = dsp1_get(); r1 = dsp1_get(); r2 = dsp1_get();
            /* Target truncates to whole ground units, about 2 pixels each here */
            CHECK(close_to(r0, hv[k][0], 8) && close_to(r1, hv[k][1], 8),
                  "round trip aas=%d (%d,%d) -> (%d,%d) m=%d",
                  a, hv[k][0], hv[k][1], r0, r1, r2);
        }

        /* raster streams A,B,C,D with the line advancing, until $8000 */
        dsp1_cmd(0x0A, (s16[]){ -40 }, 1);
        s16 la = dsp1_get(), lb = dsp1_get(), lc = dsp1_get(), ld = dsp1_get();
        s16 na = dsp1_get(), nb = dsp1_get(), nc = dsp1_get(), nd = dsp1_get();
        CHECK(la != 0 || lb != 0, "raster gave nothing");
        CHECK(la != na || lb != nb || lc != nc || ld != nd, "raster did not advance");
        for (int i = 0; i < 3; i++) (void)dsp1_get();
        dsp1_word((s16)0x8000);
        CHECK(snes_dsp1_read(1) & 0x04, "raster did not terminate");
    }
}

int main(void)
{
    test_cpu_ops();
    test_timer();
    test_dsp_output();
    test_echo();
    test_dsp1();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all APU/DSP-1 checks passed\n");
    return 0;
}

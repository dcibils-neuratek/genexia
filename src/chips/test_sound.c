/*
 * test_sound.c - sanity tests for z80.c, ym2612.c and psg.c.
 * Build: clang -O2 -Wall -Wextra -std=c11 src/test_sound.c src/z80.c src/ym2612.c src/psg.c -lm
 * Usage: test_sound [out.wav]
 */
#include "genesis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 ram[65536];
static u8 io_last_port, io_last_val;
u8 z80_mem_read(u16 a) { return ram[a]; }
void z80_mem_write(u16 a, u8 v) { ram[a] = v; }
u8 z80_io_read(u16 p) { (void)p; return 0x5A; }
void z80_io_write(u16 p, u8 v) { io_last_port = (u8)p; io_last_val = v; }

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void load(const u8 *p, size_t n, u16 at)
{
    memcpy(ram + at, p, n);
}
static void boot(const u8 *p, size_t n)
{
    memset(ram, 0, sizeof ram);
    load(p, n, 0);
    z80_reset();
}
#define STK_F ram[0x7FFE]
#define STK_A ram[0x7FFF]

static void test_z80(void)
{
    /* ADD A,n overflow: 0x7F + 1 */
    { const u8 p[] = { 0x31,0x00,0x80, 0x3E,0x7F, 0xC6,0x01, 0xF5, 0x76 };
      boot(p, sizeof p); z80_execute(200);
      CHECK(STK_A == 0x80, "ADD A=%02X", STK_A);
      CHECK((STK_F & 0xD7) == 0x94, "ADD F=%02X", STK_F); }
    /* SUB n with borrow */
    { const u8 p[] = { 0x31,0x00,0x80, 0x3E,0x10, 0xD6,0x20, 0xF5, 0x76 };
      boot(p, sizeof p); z80_execute(200);
      CHECK(STK_A == 0xF0, "SUB A=%02X", STK_A);
      CHECK((STK_F & 0xD7) == 0x83, "SUB F=%02X", STK_F); }
    /* DAA after ADD: 15+27 = 42 */
    { const u8 p[] = { 0x31,0x00,0x80, 0x3E,0x15, 0xC6,0x27, 0x27, 0xF5, 0x76 };
      boot(p, sizeof p); z80_execute(200);
      CHECK(STK_A == 0x42, "DAA add A=%02X", STK_A);
      CHECK((STK_F & 0x01) == 0, "DAA add C"); }
    /* DAA after SUB: 42-15 = 27 */
    { const u8 p[] = { 0x31,0x00,0x80, 0x3E,0x42, 0xD6,0x15, 0x27, 0xF5, 0x76 };
      boot(p, sizeof p); z80_execute(200);
      CHECK(STK_A == 0x27, "DAA sub A=%02X", STK_A);
      CHECK((STK_F & 0x03) == 0x02, "DAA sub F=%02X", STK_F); }
    /* DAA with carry out: 99+01 = 00 C */
    { const u8 p[] = { 0x31,0x00,0x80, 0x3E,0x99, 0xC6,0x01, 0x27, 0xF5, 0x76 };
      boot(p, sizeof p); z80_execute(200);
      CHECK(STK_A == 0x00 && (STK_F & 0x41) == 0x41, "DAA carry A=%02X F=%02X", STK_A, STK_F); }
    /* LDIR 5 bytes, then PUSH BC; check timing too */
    { const u8 p[] = { 0x31,0x00,0x80, 0x21,0x00,0x90, 0x11,0x00,0xA0, 0x01,0x05,0x00,
                       0xED,0xB0, 0xC5, 0x76 };
      boot(p, sizeof p);
      for (int i = 0; i < 5; i++) ram[0x9000 + i] = (u8)(0x11 * (i + 1));
      int t = z80_execute(10 + 10 + 10 + 10 + 21 * 4 + 16 + 11 + 4);
      CHECK(t == 10 + 10 + 10 + 10 + 21 * 4 + 16 + 11 + 4, "LDIR timing %d", t);
      CHECK(memcmp(ram + 0x9000, ram + 0xA000, 5) == 0 && ram[0xA005] == 0, "LDIR copy");
      CHECK(ram[0x7FFE] == 0 && ram[0x7FFF] == 0, "LDIR BC"); }
    /* CALL / RET */
    { const u8 p[] = { 0x31,0x00,0x80, 0xCD,0x10,0x00, 0x32,0x00,0x70, 0x76 };
      const u8 sub[] = { 0x3E,0x55, 0xC9 };
      boot(p, sizeof p); load(sub, sizeof sub, 0x10);
      z80_execute(200);
      CHECK(ram[0x7000] == 0x55, "CALL/RET %02X", ram[0x7000]);
      CHECK(ram[0x7FFE] == 0x06 && ram[0x7FFF] == 0x00, "CALL return addr"); }
    /* SBC HL,DE / ADC */
    { const u8 p[] = { 0x31,0x00,0x80, 0x21,0x00,0x80, 0x11,0x01,0x00, 0xB7, 0xED,0x52,
                       0xE5, 0xF5, 0x76 };
      boot(p, sizeof p); z80_execute(300);
      CHECK(ram[0x7FFE] == 0xFF && ram[0x7FFF] == 0x7F, "SBC HL result");
      /* 0x8000 - 1: overflow set, H set, N set, no C */
      CHECK((ram[0x7FFC] & 0xD7) == 0x16, "SBC HL F=%02X", ram[0x7FFC]); }
    /* IX: LD IX,nn; LD (IX+2),n; RLC (IX+2); LD A,(IX+2); LD IXH,A? */
    { const u8 p[] = { 0x31,0x00,0x80, 0xDD,0x21,0x00,0x90, 0xDD,0x36,0x02,0xAB,
                       0xDD,0xCB,0x02,0x06, 0xDD,0x7E,0x02, 0xDD,0x67, 0xDD,0xE5, 0xF5, 0x76 };
      boot(p, sizeof p); int t = z80_execute(10 + 14 + 19 + 23 + 19 + 8 + 15 + 11 + 4);
      CHECK(t == 10 + 14 + 19 + 23 + 19 + 8 + 15 + 11 + 4, "IX timing %d", t);
      CHECK(ram[0x9002] == 0x57, "RLC (IX+2) = %02X", ram[0x9002]);
      CHECK(ram[0x7FFF] == 0x57 && ram[0x7FFE] == 0x00, "LD IXH,A -> IX=%02X%02X", ram[0x7FFF], ram[0x7FFE]);
      CHECK(ram[0x7FFD] == 0x57, "LD A,(IX+2)"); }
    /* OUT (n),A */
    { const u8 p[] = { 0x3E,0x42, 0xD3,0x7F, 0x76 };
      boot(p, sizeof p); z80_execute(100);
      CHECK(io_last_port == 0x7F && io_last_val == 0x42, "OUT"); }
    /* IM1 interrupt from HALT, EI delay */
    { const u8 p[] = { 0xC3,0x00,0x01 };
      const u8 isr[] = { 0x3E,0x99, 0x32,0x01,0x70, 0xFB, 0xED,0x4D };
      const u8 main_[] = { 0x31,0x00,0x80, 0xED,0x56, 0xFB, 0x76,
                           0x3A,0x01,0x70, 0x32,0x02,0x70, 0x76 };
      boot(p, sizeof p); load(isr, sizeof isr, 0x38); load(main_, sizeof main_, 0x100);
      z80_execute(200);
      CHECK(ram[0x7001] == 0, "no irq yet");
      z80_set_irq(1);
      z80_execute(1);
      z80_set_irq(0);
      z80_execute(300);
      CHECK(ram[0x7001] == 0x99 && ram[0x7002] == 0x99, "IM1 handler ran (%02X %02X)", ram[0x7001], ram[0x7002]); }
    /* EI delay: EI; NOP with irq asserted -> irq taken after the instruction following EI */
    { const u8 p[] = { 0x31,0x00,0x80, 0xED,0x56, 0xFB, 0x3E,0x01, 0x3E,0x02, 0x76 };
      const u8 isr[] = { 0x32,0x00,0x70, 0x76 };
      boot(p, sizeof p); load(isr, sizeof isr, 0x38);
      z80_set_irq(1);
      z80_execute(300);
      z80_set_irq(0);
      CHECK(ram[0x7000] == 0x01, "EI delay: A at irq = %02X", ram[0x7000]); }
    /* plain cycle accounting */
    { const u8 p[] = { 0x00 };
      boot(p, sizeof p);
      CHECK(z80_execute(4) == 4, "NOP 4");
      CHECK(z80_execute(10) == 12, "3 NOPs 12"); }
}

/* ------------------------------------------------------------------ */
static void ymw(int part, u8 r, u8 v) { ym2612_write(part * 2, r); ym2612_write(part * 2 + 1, v); }

static void ym_patch(int ch)
{
    int part = ch / 3, c = ch % 3;
    ymw(part, 0xB0 + c, 0x32);           /* FB 6, ALG 2 */
    ymw(part, 0xB4 + c, 0xC0);
    static const u8 tl[4] = { 0x20, 0x28, 0x24, 0x00 };
    for (int s = 0; s < 4; s++) {
        int r = c + s * 4;
        ymw(part, 0x30 + r, 0x01 + (s == 1 ? 2 : 0));  /* MUL */
        ymw(part, 0x40 + r, tl[s]);
        ymw(part, 0x50 + r, 0x1F);       /* AR 31 */
        ymw(part, 0x60 + r, 0x08);       /* D1R */
        ymw(part, 0x70 + r, 0x02);       /* D2R */
        ymw(part, 0x80 + r, 0x2F);       /* SL 2, RR 15 */
        ymw(part, 0x90 + r, 0x00);
    }
}

static int peak_of(const s16 *b, int n, int stride)
{
    int pk = 0;
    for (int i = 0; i < n; i += stride) { int v = abs(b[i]); if (v > pk) pk = v; }
    return pk;
}

static void test_ym(void)
{
    static s16 buf[2 * 8192];
    ym2612_reset();
    /* pure sine: alg 7, only op4 audible */
    ymw(0, 0x22, 0); ymw(0, 0x27, 0); ymw(0, 0x2B, 0);
    ymw(0, 0xB0, 0x07); ymw(0, 0xB4, 0xC0);
    for (int s = 0; s < 4; s++) {
        ymw(0, 0x30 + s * 4, 0x01);
        ymw(0, 0x40 + s * 4, s == 3 ? 0x00 : 0x7F);
        ymw(0, 0x50 + s * 4, 0x1F);
        ymw(0, 0x60 + s * 4, 0x00);
        ymw(0, 0x70 + s * 4, 0x00);
        ymw(0, 0x80 + s * 4, 0x0F);
    }
    ymw(0, 0xA4, 0x22); ymw(0, 0xA0, 0x69);  /* block 4, fnum 0x269 -> ~250.6 Hz */
    ym2612_render(buf, 1024);
    CHECK(peak_of(buf, 2048, 1) == 0, "silent before key on");
    ymw(0, 0x28, 0xF0);
    ym2612_render(buf, 8192);
    int pk = peak_of(buf, 2 * 8192, 2);
    CHECK(pk > 2000 && pk < 4000, "FM sine peak %d", pk);
    int zc = 0;
    for (int i = 2 * 2048; i < 2 * 8192 - 2; i += 2)
        if ((buf[i] < 0) != (buf[i + 2] < 0)) zc++;
    double hz = zc / 2.0 / (6144.0 / YM_SAMPLE_RATE);
    CHECK(hz > 240 && hz < 260, "FM sine freq %.1f Hz", hz);
    /* key off -> release to silence (RR 15 is fast) */
    ymw(0, 0x28, 0x00);
    ym2612_render(buf, 4096);
    CHECK(peak_of(buf + 2 * 2048, 2 * 2048, 1) == 0, "release to silence");

    /* Timer A: TA = 1000 -> overflow after 24 samples */
    ym2612_reset();
    ymw(0, 0x24, 1000 >> 2); ymw(0, 0x25, 1000 & 3);
    ymw(0, 0x27, 0x05);
    ym2612_render(buf, 23);
    CHECK((ym2612_read_status() & 1) == 0, "timer A early");
    ym2612_render(buf, 1);
    CHECK((ym2612_read_status() & 1) == 1, "timer A overflow");
    ymw(0, 0x27, 0x15);
    CHECK((ym2612_read_status() & 1) == 0, "timer A reset");
    ym2612_render(buf, 23);
    CHECK((ym2612_read_status() & 1) == 0, "timer A reload early");
    ym2612_render(buf, 1);
    CHECK((ym2612_read_status() & 1) == 1, "timer A second overflow");
    /* Timer B: TB = 0xFE -> 32 samples */
    ymw(0, 0x26, 0xFE);
    ymw(0, 0x27, 0x0A | 0x30);
    ym2612_render(buf, 31);
    CHECK((ym2612_read_status() & 2) == 0, "timer B early");
    ym2612_render(buf, 1);
    CHECK((ym2612_read_status() & 2) == 2, "timer B overflow");
    CHECK((ym2612_read_status() & 1) == 0, "timer A not running after load bit cleared");

    /* DAC */
    ym2612_reset();
    ymw(0, 0x2B, 0x80); ymw(1, 0xB6, 0xC0); ymw(0, 0x2A, 0xFF);
    ym2612_render(buf, 4);
    CHECK(buf[6] == 254 * 11, "DAC level %d", buf[6]);
}

static void test_psg(void)
{
    static s16 buf[8192];
    psg_reset();
    psg_render(buf, 1000);
    CHECK(peak_of(buf, 1000, 1) == 0, "PSG silent at reset");
    psg_write(0x80 | 0x0E); psg_write(0x0F);  /* ch0 period 0xFE -> 440 Hz */
    psg_write(0x90 | 0x00);                   /* ch0 vol max */
    psg_render(buf, 8192);
    int pk = peak_of(buf, 8192, 1);
    CHECK(pk > 2000 && pk <= 2560, "PSG tone peak %d", pk);
    int zc = 0;
    for (int i = 0; i < 8191; i++) if ((buf[i] < 0) != (buf[i + 1] < 0)) zc++;
    double hz = zc / 2.0 / (8192.0 / YM_SAMPLE_RATE);
    CHECK(hz > 430 && hz < 450, "PSG freq %.1f", hz);
    psg_write(0x9F);
    psg_write(0xE4); psg_write(0xF0);          /* white noise, rate 0, vol max */
    psg_render(buf, 8192);
    pk = peak_of(buf, 8192, 1);
    CHECK(pk > 1500, "PSG noise peak %d", pk);
}

/* ------------------------------------------------------------------ */
static void put32(FILE *f, u32 v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, u16 v) { fputc(v & 255, f); fputc(v >> 8, f); }

static void write_wav(const char *path)
{
    const int n = YM_SAMPLE_RATE * 4;
    s16 *fm = malloc(sizeof(s16) * 2 * n), *ps = malloc(sizeof(s16) * n);
    ym2612_reset(); psg_reset();
    for (int c = 0; c < 6; c++) ym_patch(c);
    ymw(0, 0x22, 0x0B);                       /* LFO on, 6.8 Hz-ish */
    ymw(0, 0xB4, 0xC3);                       /* ch0: PMS 3 vibrato */
    static const u16 fn[6] = { 0x269, 0x309, 0x39B, 0x269, 0x309, 0x39B };
    static const u8 bl[6] = { 3, 3, 3, 4, 4, 4 };
    int pos = 0;
    psg_write(0x80 | 0x0E); psg_write(0x0F); psg_write(0x96);
    for (int c = 0; c < 6; c++) {
        int part = c / 3, cc = c % 3;
        ymw(part, 0xA4 + cc, (u8)((bl[c] << 3) | (fn[c] >> 8)));
        ymw(part, 0xA0 + cc, (u8)fn[c]);
        ymw(0, 0x28, (u8)(0xF0 | (part << 2) | cc));
        ym2612_render(fm + 2 * pos, n / 8); psg_render(ps + pos, n / 8); pos += n / 8;
    }
    for (int c = 0; c < 6; c++) ymw(0, 0x28, (u8)(((c / 3) << 2) | (c % 3)));
    psg_write(0x9F);
    ym2612_render(fm + 2 * pos, n - pos); psg_render(ps + pos, n - pos);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite("RIFF", 1, 4, f); put32(f, 36 + (u32)n * 4); fwrite("WAVEfmt ", 1, 8, f);
        put32(f, 16); put16(f, 1); put16(f, 2); put32(f, YM_SAMPLE_RATE); put32(f, YM_SAMPLE_RATE * 4);
        put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, (u32)n * 4);
        int clip = 0;
        for (int i = 0; i < n; i++) for (int ch = 0; ch < 2; ch++) {
            int v = fm[2 * i + ch] + ps[i];
            if (v > 32767) { v = 32767; clip++; } if (v < -32768) { v = -32768; clip++; }
            put16(f, (u16)(s16)v);
        }
        fclose(f);
        printf("wrote %s (%d clipped samples, fm peak %d)\n", path, clip, peak_of(fm, 2 * n, 1));
    }
    free(fm); free(ps);
}

int main(int argc, char **argv)
{
    test_z80();
    test_ym();
    test_psg();
    if (argc > 1) write_wav(argv[1]);
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}

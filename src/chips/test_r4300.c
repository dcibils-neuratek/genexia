/*
 * test_r4300.c - unit tests for r4300.c. Hand-assembled programs in a stub
 * memory map; each ends in a self-loop and is inspected after a fixed run.
 * Pass "bench" as argv[1] to also measure interpreter speed.
 */
#include "n64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

__attribute__((nocommon, aligned(16))) u8 n64_rdram[N64_RDRAM_SIZE];
__attribute__((nocommon, aligned(16))) u8 n64_sp_mem[0x2000];

/* stub I/O: 0x04000000-0x04001FFF is SP memory, the rest logs last write */
static u32 io_last_pa, io_last_val;
#define SP(pa) ((pa) >= 0x04000000u && (pa) < 0x04002000u)
static u32 sp_r32(u32 pa) {
    u8 *p = n64_sp_mem + (pa - 0x04000000u);
    return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
u8  n64_read8(u32 pa)  { return SP(pa) ? n64_sp_mem[pa - 0x04000000u] : 0xAB; }
u16 n64_read16(u32 pa) { (void)pa; return 0xABCD; }
u32 n64_read32(u32 pa) { return SP(pa) ? sp_r32(pa) : 0x12345678u; }
u64 n64_read64(u32 pa) { (void)pa; return 0x0123456789ABCDEFull; }
void n64_write8(u32 pa, u8 v)   { io_last_pa = pa; io_last_val = v; }
void n64_write16(u32 pa, u16 v) { io_last_pa = pa; io_last_val = v; }
void n64_write32(u32 pa, u32 v) { io_last_pa = pa; io_last_val = v; }
void n64_write64(u32 pa, u64 v) { io_last_pa = pa; io_last_val = (u32)v; }

/* ---------------------------------------------------------- tiny assembler */
static u32 asm_pa;
static void at(u32 pa) { asm_pa = pa; }
static void e(u32 w) {
    u8 *p = n64_rdram + asm_pa;
    p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
    asm_pa += 4;
}
static void poke32(u32 pa, u32 w) { u32 s = asm_pa; at(pa); e(w); at(s); }
static u32 peek32(u32 pa) {
    u8 *p = n64_rdram + pa;
    return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

#define I(op, rs, rt, imm) ((u32)(op) << 26 | (u32)(rs) << 21 | (u32)(rt) << 16 | ((u32)(imm) & 0xFFFFu))
#define R(fn, rs, rt, rd, sa) ((u32)(rs) << 21 | (u32)(rt) << 16 | (u32)(rd) << 11 | (u32)(sa) << 6 | (u32)(fn))
#define NOP 0u
#define LUI(rt, imm)        I(15, 0, rt, imm)
#define ORI(rt, rs, imm)    I(13, rs, rt, imm)
#define ADDIU(rt, rs, imm)  I(9, rs, rt, imm)
#define DADDIU(rt, rs, imm) I(25, rs, rt, imm)
#define ADD(rd, rs, rt)     R(32, rs, rt, rd, 0)
#define ADDU(rd, rs, rt)    R(33, rs, rt, rd, 0)
#define DADDU(rd, rs, rt)   R(45, rs, rt, rd, 0)
#define SLL(rd, rt, sa)     R(0, 0, rt, rd, sa)
#define DSLL32(rd, rt, sa)  R(60, 0, rt, rd, sa)
#define DSRA32(rd, rt, sa)  R(63, 0, rt, rd, sa)
#define DMULTU(rs, rt)      R(29, rs, rt, 0, 0)
#define DMULT(rs, rt)       R(28, rs, rt, 0, 0)
#define DDIV(rs, rt)        R(30, rs, rt, 0, 0)
#define MFHI(rd)            R(16, 0, 0, rd, 0)
#define MFLO(rd)            R(18, 0, 0, rd, 0)
#define JR(rs)              R(8, rs, 0, 0, 0)
#define SYSCALL             12u
#define BEQ(rs, rt, off)    I(4, rs, rt, off)
#define BNE(rs, rt, off)    I(5, rs, rt, off)
#define BEQL(rs, rt, off)   I(20, rs, rt, off)
#define BNEL(rs, rt, off)   I(21, rs, rt, off)
#define LW(rt, off, b)      I(35, b, rt, off)
#define LB(rt, off, b)      I(32, b, rt, off)
#define LBU(rt, off, b)     I(36, b, rt, off)
#define LD(rt, off, b)      I(55, b, rt, off)
#define SW(rt, off, b)      I(43, b, rt, off)
#define LWL(rt, off, b)     I(34, b, rt, off)
#define LWR(rt, off, b)     I(38, b, rt, off)
#define SWL(rt, off, b)     I(42, b, rt, off)
#define SWR(rt, off, b)     I(46, b, rt, off)
#define LDL(rt, off, b)     I(26, b, rt, off)
#define LDR(rt, off, b)     I(27, b, rt, off)
#define LL(rt, off, b)      I(48, b, rt, off)
#define SC(rt, off, b)      I(56, b, rt, off)
#define MFC0(rt, rd)        (16u << 26 | 0u << 21 | (u32)(rt) << 16 | (u32)(rd) << 11)
#define MTC0(rt, rd)        (16u << 26 | 4u << 21 | (u32)(rt) << 16 | (u32)(rd) << 11)
#define ERET                0x42000018u
#define TLBWI               0x42000002u
#define TLBP                0x42000008u
#define TLBR                0x42000001u
#define MTC1(rt, fs)        (17u << 26 | 4u << 21 | (u32)(rt) << 16 | (u32)(fs) << 11)
#define MFC1(rt, fs)        (17u << 26 | 0u << 21 | (u32)(rt) << 16 | (u32)(fs) << 11)
#define CTC1(rt, fs)        (17u << 26 | 6u << 21 | (u32)(rt) << 16 | (u32)(fs) << 11)
#define CFC1(rt, fs)        (17u << 26 | 2u << 21 | (u32)(rt) << 16 | (u32)(fs) << 11)
#define FP(fmt, fn, ft, fs, fd) (17u << 26 | (u32)(fmt) << 21 | (u32)(ft) << 16 | (u32)(fs) << 11 | (u32)(fd) << 6 | (u32)(fn))
#define BC1T(off)           (17u << 26 | 8u << 21 | 1u << 16 | ((u32)(off) & 0xFFFFu))
#define BC1F(off)           (17u << 26 | 8u << 21 | 0u << 16 | ((u32)(off) & 0xFFFFu))
#define BC1FL(off)          (17u << 26 | 8u << 21 | 2u << 16 | ((u32)(off) & 0xFFFFu))
#define LWC1(ft, off, b)    I(49, b, ft, off)
#define LDC1(ft, off, b)    I(53, b, ft, off)
#define SDC1(ft, off, b)    I(61, b, ft, off)
#define HALT() do { e(BEQ(0, 0, -1)); e(NOP); } while (0)

enum { T0 = 8, T1, T2, T3, T4, T5, T6, T7, S0 = 16, S1, S2, S3, K0 = 26, K1 = 27, RA = 31 };
enum { F_S = 16, F_D = 17, F_W = 20, F_L = 21 };
enum { IDX = 0, ELO0 = 2, ELO1 = 3, CTX = 4, PMASK = 5, BADV = 8, COUNT = 9,
       EHI = 10, COMPARE = 11, STATUS = 12, CAUSE = 13, EPC = 14 };

#define ST_KERNEL 0x34000000u    /* CU0 CU1 FR, as IPL3 leaves it */
#define PROG 0x80001000u

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define EQ(a, b) do { unsigned long long a_ = (a), b_ = (b); checks++; if (a_ != b_) { failures++; \
    printf("FAIL line %d: %s = %llx, expected %llx\n", __LINE__, #a, a_, b_); } } while (0)

static void begin(u32 status) {
    memset(n64_rdram, 0, 0x10000);
    r4300_reset(PROG);
    r4300_cop0[STATUS] = status;
    at(PROG & 0x1FFFFFFFu);
}
static void run(int n) { r4300_run(n); }
#define G r4300_gpr
#define C r4300_cop0

/* handler at vector: k0=Cause, k1=EPC+4, s3 counts, returns past the fault */
static void handler_at(u32 pa) {
    u32 save = asm_pa;
    at(pa);
    e(MFC0(K0, CAUSE));
    e(MFC0(K1, EPC));
    e(ADDIU(K1, K1, 4));
    e(MTC0(K1, EPC));
    e(ADDIU(S3, S3, 1));
    e(ERET);
    e(NOP);
    at(save);
}

static void test_arith(void) {
    begin(ST_KERNEL);
    e(ADDIU(T0, 0, -1));                  /* -1 sign extended */
    e(LUI(T1, 0x8000));                   /* sign extended */
    e(LUI(T2, 0x7FFF)); e(ORI(T2, T2, 0xFFFF));
    e(ADDIU(T3, T2, 1));                  /* wraps to 0xFFFFFFFF80000000 */
    e(DADDIU(T4, T2, 1));                 /* no wrap, 64-bit */
    e(SLL(T5, T2, 1));                    /* 0xFFFFFFFFFFFFFFFE */
    e(DSLL32(T6, T0, 4));
    e(DSRA32(T7, T6, 4));
    e(ADDU(S0, T1, T1));                  /* 32-bit add: 0 */
    e(DADDU(S1, T1, T1));
    e(ADDIU(0, 0, 5));                    /* r0 stays 0 */
    e(ADDU(S2, 0, 0));
    HALT();
    run(200);
    EQ(G[T0], 0xFFFFFFFFFFFFFFFFull);
    EQ(G[T1], 0xFFFFFFFF80000000ull);
    EQ(G[T3], 0xFFFFFFFF80000000ull);
    EQ(G[T4], 0x80000000ull);
    EQ(G[T5], 0xFFFFFFFFFFFFFFFEull);
    EQ(G[T6], 0xFFFFFFF000000000ull);
    EQ(G[T7], 0xFFFFFFFFFFFFFFFFull);
    EQ(G[S0], 0);
    EQ(G[S1], 0xFFFFFFFF00000000ull);
    EQ(G[S2], 0);
    EQ(G[0], 0);
}

static void test_overflow(void) {
    begin(ST_KERNEL);
    handler_at(0x180);
    e(LUI(T0, 0x7FFF)); e(ORI(T0, T0, 0xFFFF));
    e(ADDIU(T1, 0, 1));
    e(ADDIU(T2, 0, 99));
    e(ADD(T2, T0, T1));                   /* traps, T2 keeps 99 */
    HALT();
    run(200);
    EQ(G[T2], 99);
    EQ((G[K0] >> 2) & 31, 12);
    EQ(G[S3], 1);
    CHECK(!(C[STATUS] & 2));
}

static void test_branches(void) {
    begin(ST_KERNEL);
    /* delay slot runs on a taken branch */
    e(ADDIU(T0, 0, 1));
    e(BEQ(0, 0, 2));
    e(ADDIU(T1, 0, 7));                   /* delay slot */
    e(ADDIU(T2, 0, 8));                   /* skipped */
    /* not-taken branch likely skips delay slot */
    e(BNEL(0, 0, 2));
    e(ADDIU(T3, 0, 9));                   /* nullified */
    e(ADDIU(T4, 0, 10));
    /* taken branch likely executes it */
    e(BEQL(0, 0, 2));
    e(ADDIU(T5, 0, 11));
    e(ADDIU(T6, 0, 12));                  /* skipped */
    /* JAL links past the delay slot */
    e(3u << 26 | ((PROG + 16 * 4) & 0x0FFFFFFFu) >> 2);
    e(ADDIU(T7, 0, 13));
    HALT();
    while (asm_pa < (PROG & 0x1FFFFFFFu) + 16 * 4) e(NOP);
    e(ADDIU(S0, 0, 14));
    HALT();
    run(200);
    EQ(G[T1], 7); EQ(G[T2], 0); EQ(G[T3], 0); EQ(G[T4], 10);
    EQ(G[T5], 11); EQ(G[T6], 0); EQ(G[T7], 13); EQ(G[S0], 14);
    EQ(G[RA], 0xFFFFFFFF80001000ull + 12 * 4);
}

static void test_unaligned(void) {
    begin(ST_KERNEL);
    poke32(0x2000, 0x11223344); poke32(0x2004, 0x55667788);
    poke32(0x2008, 0x99AABBCC); poke32(0x200C, 0xDDEEFF00);
    e(LUI(S0, 0x8000)); e(ORI(S0, S0, 0x2000));
    e(LWL(T0, 1, S0)); e(LWR(T0, 4, S0));   /* 0x22334455 */
    e(LWL(T1, 9, S0)); e(LWR(T1, 12, S0));  /* 0xAABBCCDD sign extended */
    e(LDL(T2, 3, S0)); e(LDR(T2, 10, S0));  /* 0x4455667788 99AABB */
    e(LUI(T3, 0xAABB)); e(ORI(T3, T3, 0xCCDD));
    e(SWL(T3, 0x21, S0)); e(SWR(T3, 0x24, S0));
    HALT();
    run(200);
    EQ(G[T0], 0x22334455ull);
    EQ(G[T1], 0xFFFFFFFFAABBCCDDull);
    EQ(G[T2], 0x445566778899AABBull);
    EQ(peek32(0x2020), 0x00AABBCCu);
    EQ(peek32(0x2024), 0xDD000000u);
}

static void test_mul_div(void) {
    begin(ST_KERNEL);
    e(ADDIU(T0, 0, -1));                     /* 2^64-1 */
    e(DMULTU(T0, T0));
    e(MFHI(T1)); e(MFLO(T2));
    e(ADDIU(T3, 0, 3));
    e(DMULT(T0, T3));                        /* -1 * 3 */
    e(MFHI(T4)); e(MFLO(T5));
    e(ADDIU(T6, 0, -7));
    e(ADDIU(T7, 0, 2));
    e(DDIV(T6, T7));
    e(MFHI(S0)); e(MFLO(S1));
    HALT();
    run(400);
    EQ(G[T1], 0xFFFFFFFFFFFFFFFEull);
    EQ(G[T2], 1);
    EQ(G[T4], 0xFFFFFFFFFFFFFFFFull);
    EQ(G[T5], 0xFFFFFFFFFFFFFFFDull);
    EQ(G[S0], 0xFFFFFFFFFFFFFFFFull);
    EQ(G[S1], 0xFFFFFFFFFFFFFFFDull);
}

static void test_ll_sc_io(void) {
    begin(ST_KERNEL);
    e(LUI(S0, 0x8000)); e(ORI(S0, S0, 0x2000));
    e(LL(T0, 0, S0));
    e(ADDIU(T0, T0, 5));
    e(SC(T0, 0, S0));                        /* succeeds: T0=1 */
    e(SC(T1, 0, S0));                        /* LLbit gone: T1=0 */
    e(LUI(S1, 0xA430));                      /* KSEG1 register space */
    e(LW(T2, 4, S1));
    e(ADDIU(T3, 0, 0x55));
    e(SW(T3, 8, S1));
    e(LB(T4, 0, S1));
    e(LBU(T5, 0, S1));
    e(LD(T6, 0, S1));
    HALT();
    poke32(0x2000, 10);
    run(200);
    EQ(G[T0], 1); EQ(G[T1], 0);
    EQ(peek32(0x2000), 15);
    EQ(G[T2], 0x12345678);
    EQ(io_last_pa, 0x04300008u); EQ(io_last_val, 0x55);
    EQ(G[T4], 0xFFFFFFFFFFFFFFABull); EQ(G[T5], 0xAB);
    EQ(G[T6], 0x0123456789ABCDEFull);
}

static void test_fetch_sp(void) {
    u8 *p = n64_sp_mem + 0x1000;
    u32 code[] = { ADDIU(T0, 0, 42), BEQ(0, 0, -1), NOP };
    for (int i = 0; i < 3; i++) {
        p[i * 4] = (u8)(code[i] >> 24); p[i * 4 + 1] = (u8)(code[i] >> 16);
        p[i * 4 + 2] = (u8)(code[i] >> 8); p[i * 4 + 3] = (u8)code[i];
    }
    r4300_reset(0xA4001000u);
    r4300_cop0[STATUS] = ST_KERNEL;
    run(50);
    EQ(G[T0], 42);
}

static void test_fpu(void) {
    begin(ST_KERNEL);
    poke32(0x2000, 0x40400000);              /* 3.0f */
    poke32(0x2008, 0x40091EB8); poke32(0x200C, 0x51EB851F);   /* 3.14 */
    e(LUI(S0, 0x8000)); e(ORI(S0, S0, 0x2000));
    e(LWC1(0, 0, S0));                       /* f0 = 3.0 */
    e(LUI(T0, 0x4000)); e(MTC1(T0, 1));      /* f1 = 2.0 */
    e(FP(F_S, 0, 1, 0, 2));                  /* add.s f2 = 5 */
    e(FP(F_S, 2, 1, 2, 3));                  /* mul.s f3 = 10 */
    e(FP(F_S, 3, 1, 3, 4));                  /* div.s f4 = 5 */
    e(FP(F_S, 33, 0, 4, 5));                 /* cvt.d.s f5 */
    e(FP(F_D, 4, 0, 5, 6));                  /* sqrt.d f6 = 2.236.. */
    e(FP(F_D, 13, 0, 6, 7));                 /* trunc.w.d f7 = 2 */
    e(MFC1(T1, 7));
    e(LDC1(8, 8, S0));                       /* 3.14 */
    e(FP(F_D, 8, 0, 8, 9));                  /* round.l.d = 3 */
    e(FP(F_D, 10, 0, 8, 10));                /* ceil.l.d = 4 */
    e(FP(F_D, 15, 0, 8, 11));                /* floor.w.d = 3 */
    e(FP(F_L, 33, 0, 10, 12));               /* cvt.d.l = 4.0 */
    e(SDC1(12, 0x10, S0));
    /* C.LT.S 2 < 3 sets condition */
    e(FP(F_S, 60, 0, 1, 0));
    e(BC1T(2));
    e(ADDIU(T2, 0, 1));                      /* delay slot */
    e(ADDIU(T3, 0, 1));                      /* skipped */
    e(BC1FL(2));                             /* not taken: slot skipped */
    e(ADDIU(T4, 0, 1));
    e(FP(F_S, 62, 1, 0, 0));                 /* c.le.s 3 <= 2 false */
    e(CFC1(T5, 31));
    HALT();
    run(800);
    EQ(G[T1], 2);
    EQ(G[T2], 1); EQ(G[T3], 0); EQ(G[T4], 0);
    CHECK(!(G[T5] & (1u << 23)));
    EQ(peek32(0x2010), 0x40100000u);          /* 4.0 double hi */
}

static void test_fpu_rounding(void) {
    begin(ST_KERNEL);
    /* 2.5 -> nearest 2, zero 2, up 3, down 2; -2.5 -> nearest -2, down -3 */
    e(LUI(T0, 0x4020)); e(MTC1(T0, 0));      /* 2.5f */
    e(LUI(T0, 0xC020)); e(MTC1(T0, 1));      /* -2.5f */
    e(FP(F_S, 36, 0, 0, 2)); e(MFC1(S0, 2)); /* cvt.w nearest */
    e(ADDIU(T1, 0, 2)); e(CTC1(T1, 31));     /* RM = +inf */
    e(FP(F_S, 36, 0, 0, 2)); e(MFC1(S1, 2));
    e(ADDIU(T1, 0, 3)); e(CTC1(T1, 31));     /* RM = -inf */
    e(FP(F_S, 36, 0, 1, 2)); e(MFC1(S2, 2));
    e(FP(F_S, 12, 0, 1, 2)); e(MFC1(T2, 2)); /* round.w ignores RM: -2 */
    e(FP(F_S, 13, 0, 1, 2)); e(MFC1(T3, 2)); /* trunc.w: -2 */
    e(FP(F_S, 14, 0, 1, 2)); e(MFC1(T4, 2)); /* ceil.w: -2 */
    e(FP(F_S, 15, 0, 0, 2)); e(MFC1(T5, 2)); /* floor.w: 2 */
    /* 1/3 in single rounds differently up and down */
    e(LUI(T6, 0x3F80)); e(MTC1(T6, 3));
    e(LUI(T6, 0x4040)); e(MTC1(T6, 4));
    e(FP(F_S, 3, 4, 3, 5)); e(MFC1(T6, 5));  /* div.s RM -inf */
    e(ADDIU(T1, 0, 2)); e(CTC1(T1, 31));
    e(FP(F_S, 3, 4, 3, 5)); e(MFC1(T7, 5));  /* div.s RM +inf */
    e(CTC1(0, 31));
    HALT();
    run(400);
    EQ((u32)G[S0], 2); EQ((u32)G[S1], 3); EQ((u32)G[S2], (u32)-3);
    EQ((u32)G[T2], (u32)-2); EQ((u32)G[T3], (u32)-2);
    EQ((u32)G[T4], (u32)-2); EQ((u32)G[T5], 2);
    EQ((u32)G[T6], 0x3EAAAAAAu); EQ((u32)G[T7], 0x3EAAAAABu);
}

static void test_fpu_fr0(void) {
    begin(0x30000000u);                      /* FR=0 */
    e(LUI(T0, 0x3FF0)); e(MTC1(T0, 3));      /* high half of f2 */
    e(MTC1(0, 2));                           /* f2 = 1.0 double */
    e(FP(F_D, 0, 2, 2, 4));                  /* add.d f4 = 2.0 */
    e(MFC1(T1, 5));                          /* high half of f4 */
    e(MFC1(T2, 4));
    HALT();
    run(100);
    EQ((u32)G[T1], 0x40000000u); EQ((u32)G[T2], 0);
}

static void test_cu1(void) {
    begin(0x10000000u);                      /* CU1 off */
    handler_at(0x180);
    e(ADDIU(T0, 0, 3));
    e(MTC1(T0, 0));                          /* coprocessor unusable */
    e(ADDIU(T1, 0, 4));
    HALT();
    run(100);
    EQ((G[K0] >> 2) & 31, 11);
    EQ((G[K0] >> 28) & 3, 1);
    EQ(G[T1], 4);
}

static void test_fpe(void) {
    begin(ST_KERNEL);
    handler_at(0x180);
    e(ADDIU(T0, 0, 0x0400)); e(CTC1(T0, 31));  /* enable Z (bit 10) */
    e(LUI(T1, 0x3F80)); e(MTC1(T1, 0)); e(MTC1(0, 1));
    e(FP(F_S, 3, 1, 0, 2));                  /* 1/0: traps */
    e(CFC1(T2, 31));
    e(CTC1(0, 31));
    e(FP(F_S, 3, 1, 0, 3));                  /* untrapped: infinity */
    e(MFC1(T3, 3));
    e(CFC1(T4, 31));
    HALT();
    run(300);
    EQ((G[K0] >> 2) & 31, 15);
    CHECK(G[T2] & 0x8000);                   /* cause Z */
    EQ((u32)G[T3], 0x7F800000u);
    CHECK(G[T4] & 0x20);                     /* sticky Z flag */
}

static void test_tlb(void) {
    begin(ST_KERNEL);
    /* refill vector: map the page, retry the faulting instruction */
    at(0x000);
    e(MFC0(K0, BADV));
    e(MFC0(K1, CTX));
    e(ADDIU(S3, S3, 1));
    e(MTC0(0, PMASK));
    e(MTC0(0, IDX));
    e(LUI(T7, 0));
    e(ORI(T7, T7, (0x3000 >> 12) << 6 | 0x1E));  /* even page -> 0x3000, D V C */
    e(MTC0(T7, ELO0));
    e(ORI(T7, 0, (0x4000 >> 12) << 6 | 0x1A));   /* odd page -> 0x4000, read-only */
    e(MTC0(T7, ELO1));
    e(TLBWI);
    e(ERET);
    e(NOP);
    handler_at(0x180);
    at(PROG & 0x1FFFFFFFu);
    poke32(0x3010, 0xCAFEF00D);
    poke32(0x4020, 0xDEADBEEF);
    e(ADDIU(T0, 0, 0x42)); e(MTC0(T0, EHI));     /* ASID 0x42 */
    e(LUI(S0, 0x0040));                          /* mapped 0x00400000 */
    e(LW(T1, 0x10, S0));                         /* refill, then hit */
    e(LW(T2, 0x10, S0));
    e(ADDIU(T3, 0, 0x77));
    e(SW(T3, 0x14, S0));                         /* writable even page */
    e(LUI(T4, 0));
    e(ORI(T4, S0, 0x1020));
    e(LW(T5, 0, T4));                            /* odd page */
    e(SW(T3, 0, T4));                            /* not dirty: TLB Mod at 0x180 */
    e(MFC0(T6, EHI));
    e(TLBP);
    e(MFC0(S1, IDX));
    e(TLBR);
    e(MFC0(S2, ELO1));
    HALT();
    run(800);
    EQ(G[T1], 0xFFFFFFFFCAFEF00Dull);
    EQ(G[T2], 0xFFFFFFFFCAFEF00Dull);
    EQ(peek32(0x3014), 0x77);
    EQ(G[T5], 0xFFFFFFFFDEADBEEFull);
    EQ(peek32(0x4020), 0xDEADBEEFu);             /* store was refused */
    EQ((G[K0] >> 2) & 31, 1);                    /* Mod */
    EQ(G[S1], 0);                                /* TLBP found entry 0 */
    EQ((u32)G[S2] >> 6, 0x4);
    EQ((u32)G[T6] & 0xFF, 0x42);
    EQ(G[S3], 2);                                /* one refill, one Mod */
}

static void test_tlb_vector(void) {
    begin(ST_KERNEL);
    at(0x000); e(ADDIU(S0, 0, 1)); HALT();
    at(0x180); e(ADDIU(S1, 0, 1)); HALT();
    at(PROG & 0x1FFFFFFFu);
    e(NOP);
    e(LUI(T0, 0x0123));
    e(BEQ(0, 0, 1));
    e(LW(T1, 0x10, T0));                         /* refill in a delay slot */
    HALT();
    C[CTX] = 0x00800000u;
    run(100);
    EQ(G[S0], 1); EQ(G[S1], 0);
    EQ(C[BADV], 0x01230010u);
    EQ(C[EPC], PROG + 8);
    CHECK(C[CAUSE] & 0x80000000u);
    EQ((C[CAUSE] >> 2) & 31, 2);
    EQ(C[CTX], 0x00800000u | (0x01230010u >> 13) << 4);
    EQ(C[EHI] & ~0xFFu, 0x01230000u & ~0x1FFFu);

    /* with EXL already set a refill takes the general vector, EPC is frozen */
    begin(ST_KERNEL | 2);
    at(0x000); e(ADDIU(S0, 0, 1)); HALT();
    at(0x180); e(ADDIU(S1, 0, 1)); HALT();
    at(PROG & 0x1FFFFFFFu);
    C[EPC] = 0x12345678u;
    e(LUI(T0, 0x0123));
    e(LW(T1, 0, T0));
    HALT();
    run(30);
    EQ(G[S0], 0); EQ(G[S1], 1);
    EQ(C[EPC], 0x12345678u);

    /* a matching entry without V is TLB invalid at the general vector */
    begin(ST_KERNEL);
    at(0x000); e(ADDIU(S0, 0, 1)); HALT();
    at(0x180); e(ADDIU(S1, 0, 1)); HALT();
    at(PROG & 0x1FFFFFFFu);
    e(LUI(T0, 0x0123));
    e(MTC0(T0, EHI));
    e(ADDIU(T1, 0, 0x1C));                       /* D, cached, not valid */
    e(MTC0(T1, ELO0));
    e(MTC0(T1, ELO1));
    e(TLBWI);
    e(SW(T1, 0, T0));
    HALT();
    run(40);
    EQ(G[S0], 0); EQ(G[S1], 1);
    EQ((C[CAUSE] >> 2) & 31, 3);
    EQ(C[BADV], 0x01230000u);
}

static void test_addr_error(void) {
    begin(ST_KERNEL);
    handler_at(0x180);
    e(LUI(S0, 0x8000));
    e(LW(T0, 2, S0));
    HALT();
    run(100);
    EQ((G[K0] >> 2) & 31, 4);
    EQ(C[BADV], 0x80000002u);
}

static void test_timer(void) {
    begin(ST_KERNEL);
    at(0x180);
    e(MFC0(K0, CAUSE));
    e(MFC0(K1, COUNT));
    e(ADDIU(T1, 0, 1000));
    e(MTC0(T1, COMPARE));                        /* clears IP7, re-arms far away */
    e(ADDIU(S3, S3, 1));
    e(ERET);
    e(NOP);
    at(PROG & 0x1FFFFFFFu);
    e(ADDIU(T0, 0, 50));
    e(MTC0(T0, COMPARE));
    e(MTC0(0, COUNT));
    e(LUI(T2, 0x3400));                          /* keep CU, enable IE + IM7 */
    e(ORI(T2, T2, 0x8001));
    e(MTC0(T2, STATUS));
    HALT();
    run(95);
    EQ(G[S3], 0);
    run(30);
    EQ(G[S3], 1);
    CHECK(G[K0] & 0x8000);
    CHECK(!(C[CAUSE] & 0x8000));
    EQ(C[COMPARE], 1000);
    EQ(G[K1] >= 50 && G[K1] < 60, 1);
    CHECK(!(C[STATUS] & 2));                     /* ERET cleared EXL */
    u32 c0 = C[COUNT];
    run(1000);
    EQ(C[COUNT] - c0, 500);
}

static void test_mi_irq(void) {
    begin(ST_KERNEL | 0x0401);                   /* IE + IM2 */
    at(0x180);
    e(MFC0(K0, CAUSE));
    e(ADDIU(S3, S3, 1));
    e(ERET);
    at(PROG & 0x1FFFFFFFu);
    e(ADDIU(T0, 0, 0));
    e(BEQ(0, 0, -1));
    e(ADDIU(T0, T0, 1));
    run(40);
    EQ(G[S3], 0);
    r4300_set_irq(1);
    run(2);                                      /* line still high: stop before ERET */
    EQ(G[S3], 1);
    CHECK(G[K0] & 0x400);
    EQ((G[K0] >> 2) & 31, 0);
    r4300_set_irq(0);
    r4300_run(40);
    EQ(G[S3], 1);
}

static void test_syscall_bd_random(void) {
    begin(ST_KERNEL);
    at(0x180);
    e(MFC0(K0, CAUSE)); e(MFC0(K1, EPC));
    e(ADDIU(K1, K1, 8));                         /* skip branch + slot */
    e(MTC0(K1, EPC));
    e(ERET);
    at(PROG & 0x1FFFFFFFu);
    e(BEQ(0, 0, 3));
    e(SYSCALL);                                  /* in delay slot */
    e(ADDIU(T1, 0, 5));                          /* resumes here */
    e(ADDIU(T2, 0, 6));
    e(MFC0(T3, 1));
    e(ADDIU(T4, 0, 20));
    e(MTC0(T4, 6));                              /* Wired=20 */
    e(MFC0(T5, 1));
    HALT();
    run(100);
    CHECK(G[K0] & 0x80000000u);
    EQ((G[K0] >> 2) & 31, 8);
    EQ(G[T1], 5);
    CHECK(G[T3] <= 31);
    CHECK(G[T5] >= 20 && G[T5] <= 31);
    u32 seen = 0;
    for (int i = 0; i < 40; i++) {
        run(1);
        CHECK(C[1] >= 20 && C[1] <= 31);
        seen |= 1u << C[1];
    }
    CHECK(seen & (1u << 20));
    CHECK(seen & (1u << 31));
}

static void bench(void) {
    begin(ST_KERNEL);
    poke32(0x2000, 1);
    e(LUI(S0, 0x8000)); e(ORI(S0, S0, 0x2000));
    e(LUI(T7, 0x3F80)); e(MTC1(T7, 1));
    u32 loop = asm_pa;
    e(LW(T0, 0, S0));
    e(ADDU(T1, T0, T2));
    e(SLL(T2, T1, 1));
    e(ADDIU(T3, T3, 3));
    e(SW(T1, 4, S0));
    e(ORI(T4, T3, 0x55));
    e(ADDU(T5, T4, T1));
    e(FP(F_S, 0, 1, 2, 2));
    e(BNE(T3, 0, (int)(loop - asm_pa) / 4 - 1));
    e(ADDU(T6, T5, T3));
    clock_t t0 = clock();
    u64 total = 0;
    for (int i = 0; i < 2000; i++) total += (u64)r4300_run(1000000);
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: %.0f M emulated cycles/s (2.0 billion cycles total)\n",
           (double)total / s / 1e6);
    printf("       about %.0f MIPS on this load\n", (double)total / 1.3 / s / 1e6);
}

int main(int argc, char **argv) {
    test_arith();
    test_overflow();
    test_branches();
    test_unaligned();
    test_mul_div();
    test_ll_sc_io();
    test_fetch_sp();
    test_fpu();
    test_fpu_rounding();
    test_fpu_fr0();
    test_cu1();
    test_fpe();
    test_tlb();
    test_tlb_vector();
    test_addr_error();
    test_timer();
    test_mi_irq();
    test_syscall_bd_random();
    printf("%d checks, %d failures\n", checks, failures);
    if (argc > 1 && !strcmp(argv[1], "bench")) bench();
    return failures ? 1 : 0;
}

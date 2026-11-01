/*
 * test_rsp.c - RSP tests: hand-assembled programs in IMEM, with stubs for the
 * machine. Includes rsp.c directly so it can look at the static state.
 *
 *   clang -O2 -Wall -Wextra -std=c11 -Isrc -Isrc/backends/n64 \
 *         src/chips/test_rsp.c -o test_rsp
 */
#include "rsp.c"
#include <stdio.h>
#include <time.h>

u8 n64_sp_mem[0x2000];

static int halted, breaks;
static u32 cop0[16];
u32 n64_rsp_cop0_read(int reg) { return cop0[reg]; }
void n64_rsp_cop0_write(int reg, u32 v)
{
    cop0[reg] = v;
    if (reg == 4 && (v & 2)) halted = 1;   /* set halt */
}
void n64_sp_break(void) { breaks++; halted = 1; }
int n64_sp_halted(void) { return halted; }

/* ----------------------------------------------------------------- assembler */
#define R_(f, rs, rt, rd, sa) ((u32)(rs) << 21 | (u32)(rt) << 16 | (u32)(rd) << 11 | (u32)(sa) << 6 | (f))
#define I_(o, rs, rt, imm) ((u32)(o) << 26 | (u32)(rs) << 21 | (u32)(rt) << 16 | ((u32)(imm) & 0xFFFF))
#define ADDIU(t, s, i) I_(9, s, t, i)
#define ORI(t, s, i)   I_(0xD, s, t, i)
#define LUI(t, i)      I_(0xF, 0, t, i)
#define SLTI(t, s, i)  I_(0xA, s, t, i)
#define SLTIU(t, s, i) I_(0xB, s, t, i)
#define ADDU(d, s, t)  R_(0x21, s, t, d, 0)
#define SUBU(d, s, t)  R_(0x23, s, t, d, 0)
#define NOR(d, s, t)   R_(0x27, s, t, d, 0)
#define SLT(d, s, t)   R_(0x2A, s, t, d, 0)
#define SLL(d, t, a)   R_(0, 0, t, d, a)
#define SRA(d, t, a)   R_(3, 0, t, d, a)
#define JR(s)          R_(8, s, 0, 0, 0)
#define JALR(d, s)     R_(9, s, 0, d, 0)
#define BREAK          0x0000000Du
#define NOP            0u
#define J(a)           ((2u << 26) | ((a) >> 2))
#define JAL(a)         ((3u << 26) | ((a) >> 2))
#define BEQ(s, t, o)   I_(4, s, t, o)
#define BNE(s, t, o)   I_(5, s, t, o)
#define BLTZAL(s, o)   I_(1, s, 0x10, o)
#define BGEZAL(s, o)   I_(1, s, 0x11, o)
#define LB(t, s, i)    I_(0x20, s, t, i)
#define LH(t, s, i)    I_(0x21, s, t, i)
#define LW(t, s, i)    I_(0x23, s, t, i)
#define LBU(t, s, i)   I_(0x24, s, t, i)
#define SB(t, s, i)    I_(0x28, s, t, i)
#define SH(t, s, i)    I_(0x29, s, t, i)
#define SW(t, s, i)    I_(0x2B, s, t, i)
#define MTC0(t, d)     (0x40800000u | (u32)(t) << 16 | (u32)(d) << 11)
#define MFC0(t, d)     (0x40000000u | (u32)(t) << 16 | (u32)(d) << 11)
#define MFC2(t, v, e)  (0x48000000u | (u32)(t) << 16 | (u32)(v) << 11 | (u32)(e) << 7)
#define CFC2(t, c)     (0x48400000u | (u32)(t) << 16 | (u32)(c) << 11)
#define MTC2(t, v, e)  (0x48800000u | (u32)(t) << 16 | (u32)(v) << 11 | (u32)(e) << 7)
#define CTC2(t, c)     (0x48C00000u | (u32)(t) << 16 | (u32)(c) << 11)
#define VOP(f, d, s, t, e) (0x4A000000u | (u32)(e) << 21 | (u32)(t) << 16 | (u32)(s) << 11 | (u32)(d) << 6 | (f))
#define LVEC(fmt, t, e, off, b) (0xC8000000u | (u32)(b) << 21 | (u32)(t) << 16 | (u32)(fmt) << 11 | (u32)(e) << 7 | ((u32)(off) & 0x7F))
#define SVEC(fmt, t, e, off, b) (0xE8000000u | (u32)(b) << 21 | (u32)(t) << 16 | (u32)(fmt) << 11 | (u32)(e) << 7 | ((u32)(off) & 0x7F))
enum { F_B, F_S, F_L, F_D, F_Q, F_R, F_P, F_U, F_H, F_F, F_W, F_T };
enum {
    VMULF = 0, VMULU = 1, VMUDL = 4, VMUDM = 5, VMUDN = 6, VMUDH = 7, VMACF = 8, VMACU = 9,
    VMADL = 12, VMADM = 13, VMADN = 14, VMADH = 15, VADD = 16, VSUB = 17, VABS = 19,
    VADDC = 20, VSUBC = 21, VSAR = 29, VLT = 32, VEQ = 33, VNE = 34, VGE = 35, VCL = 36,
    VCH = 37, VCR = 38, VMRG = 39, VAND = 40, VNAND = 41, VOR = 42, VNOR = 43, VXOR = 44,
    VNXOR = 45, VRCP = 48, VRCPL = 49, VRCPH = 50, VMOV = 51, VRSQ = 52, VRSQL = 53, VRSQH = 54,
};

/* ------------------------------------------------------------------ harness */
static int failures, checks;

#define CHECK(got, want) do {                                                  \
    unsigned long long g_ = (unsigned long long)(got), w_ = (unsigned long long)(want); \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
        failures++;                                                            \
        printf("FAIL %s:%d %s = 0x%llx, want 0x%llx\n", __func__, __LINE__, #got, g_, w_); \
    }                                                                          \
} while (0)

static void put(u32 addr, u32 word)
{
    u8 *m = n64_sp_mem + 0x1000 + (addr & 0xFFF);
    m[0] = (u8)(word >> 24); m[1] = (u8)(word >> 16); m[2] = (u8)(word >> 8); m[3] = (u8)word;
}

static void load_at(u32 addr, const u32 *code, int n)
{
    for (int i = 0; i < n; i++) put(addr + (u32)i * 4, code[i]);
}

static int run_prog(const u32 *code, int n)
{
    load_at(0, code, n);
    put((u32)n * 4, BREAK);
    halted = 0;
    rsp_set_pc(0);
    return rsp_run(100000);
}
#define RUN(...) do {                                                          \
    static const u32 prog_[] = {__VA_ARGS__};                                  \
    run_prog(prog_, (int)(sizeof prog_ / sizeof prog_[0]));                    \
} while (0)

static void fresh(void)
{
    rsp_reset();
    memset(n64_sp_mem, 0, sizeof n64_sp_mem);
    memset(cop0, 0, sizeof cop0);
    halted = 0;
    breaks = 0;
}

static void setv(int r, u16 a, u16 b, u16 c, u16 d, u16 e, u16 f, u16 g, u16 h)
{
    u16 v[8] = {a, b, c, d, e, f, g, h};
    memcpy(R.vr[r], v, 16);
}

static void setall(int r, u16 a) { setv(r, a, a, a, a, a, a, a, a); }

static void checkv(const char *what, int line, const u16 *got, u16 a, u16 b, u16 c, u16 d, u16 e, u16 f, u16 g, u16 h)
{
    u16 want[8] = {a, b, c, d, e, f, g, h};
    checks++;
    if (memcmp(got, want, 16)) {
        failures++;
        printf("FAIL %s line %d\n  got ", what, line);
        for (int i = 0; i < 8; i++) printf(" %04x", got[i]);
        printf("\n  want");
        for (int i = 0; i < 8; i++) printf(" %04x", want[i]);
        printf("\n");
    }
}
#define CHECKV(p, ...) checkv(#p, __LINE__, p, __VA_ARGS__)

/* -------------------------------------------------------------------- tests */
static void test_scalar(void)
{
    fresh();
    RUN(LUI(1, 0x1234), ORI(1, 1, 0x5678), ADDIU(2, 0, -1), ADDU(3, 1, 2), SUBU(4, 2, 1),
        NOR(5, 0, 0), SLT(6, 2, 0), SLTI(7, 2, 0), SLTIU(8, 2, 5), SLL(9, 1, 4), SRA(10, 2, 8),
        SLL(11, 2, 28), SRA(11, 11, 28), ADDIU(0, 0, 55));
    CHECK(R.gpr[1], 0x12345678);
    CHECK(R.gpr[2], 0xFFFFFFFF);
    CHECK(R.gpr[3], 0x12345677);
    CHECK(R.gpr[4], 0xFFFFFFFFu - 0x12345678u);
    CHECK(R.gpr[5], 0xFFFFFFFF);
    CHECK(R.gpr[6], 1);
    CHECK(R.gpr[7], 1);
    CHECK(R.gpr[8], 0);
    CHECK(R.gpr[9], 0x23456780);
    CHECK(R.gpr[10], 0xFFFFFFFF);
    CHECK(R.gpr[11], 0xFFFFFFFF);
    CHECK(R.gpr[0], 0);
    CHECK(breaks, 1);
}

static void test_branches(void)
{
    /* delay slot runs, branch skips, jal links, subroutine at 0x24 */
    fresh();
    u32 code[] = {
        ADDIU(1, 0, 1), BEQ(1, 1, 2), ADDIU(2, 0, 7), ADDIU(3, 0, 9),
        JAL(0x24), ADDIU(4, 0, 3), ADDIU(5, 0, 1), J(0x34),
        NOP, JR(31), ADDIU(6, 0, 2), NOP, NOP, BREAK,
    };
    load_at(0, code, 14);
    rsp_set_pc(0);
    rsp_run(1000);
    CHECK(R.gpr[2], 7);
    CHECK(R.gpr[3], 0);
    CHECK(R.gpr[4], 3);
    CHECK(R.gpr[31], 0x18);
    CHECK(R.gpr[5], 1);
    CHECK(R.gpr[6], 2);
    CHECK(rsp_get_pc(), 0x38);

    /* BLTZAL links even when not taken, BGEZAL too */
    fresh();
    RUN(BLTZAL(0, 2), NOP, ADDIU(1, 31, 0), BGEZAL(0, 2), NOP, ADDIU(2, 0, 5), ADDIU(3, 31, 0));
    CHECK(R.gpr[1], 8);
    CHECK(R.gpr[2], 0);
    CHECK(R.gpr[3], 0x14);

    /* JALR links rd; PC wraps at 4 KB */
    fresh();
    put(0xFFC, ADDIU(1, 0, 42));
    put(0x000, BREAK);
    halted = 0;
    rsp_set_pc(0xFFC);
    rsp_run(10);
    CHECK(R.gpr[1], 42);
    CHECK(rsp_get_pc(), 4);

    fresh();
    put(0x000, ADDIU(1, 0, 0x100));
    put(0x004, JALR(2, 1));
    put(0x008, NOP);
    put(0x100, BREAK);
    rsp_set_pc(0);
    rsp_run(10);
    CHECK(R.gpr[2], 0xC);

    /* A cycle limit landing on the delay slot resumes correctly. */
    fresh();
    put(0x000, J(0x100));
    put(0x004, ADDIU(1, 0, 1));
    put(0x008, ADDIU(2, 0, 1));
    put(0x100, ADDIU(3, 0, 1));
    put(0x104, BREAK);
    rsp_set_pc(0);
    CHECK(rsp_run(1), 1);
    CHECK(R.delay, 1);
    CHECK(rsp_get_pc(), 4);
    rsp_run(100);
    CHECK(R.gpr[1], 1);
    CHECK(R.gpr[2], 0);
    CHECK(R.gpr[3], 1);

    /* BREAK in a delay slot leaves PC at the target */
    fresh();
    put(0x000, J(0x200));
    put(0x004, BREAK);
    rsp_set_pc(0);
    CHECK(rsp_run(100), 2);
    CHECK(rsp_get_pc(), 0x200);
    CHECK(R.delay, 0);
}

static void test_halt_cop0(void)
{
    fresh();
    cop0[9] = 0xCAFE;
    RUN(MFC0(1, 9), ADDIU(2, 0, 2), MTC0(2, 4), ADDIU(3, 0, 1));
    CHECK(R.gpr[1], 0xCAFE);
    CHECK(cop0[4], 2);
    CHECK(R.gpr[3], 0);
    CHECK(breaks, 0);
    CHECK(rsp_get_pc(), 12);
    CHECK(rsp_run(10), 0);     /* still halted */
}

static void test_dmem(void)
{
    fresh();
    /* Word store wrapping across 0xFFF */
    RUN(LUI(1, 0x1122), ORI(1, 1, 0x3344), ADDIU(2, 0, 0xFFE), SW(1, 2, 0), LW(3, 2, 0),
        ADDIU(4, 0, 0xFFF), SH(1, 4, 0), LH(5, 4, 0), ADDIU(6, 0, 0x1FFF), LBU(7, 6, 0));
    CHECK(n64_sp_mem[0xFFE], 0x11);
    CHECK(n64_sp_mem[0xFFF], 0x33);
    CHECK(n64_sp_mem[0x000], 0x44);
    CHECK(n64_sp_mem[0x001], 0x44);
    CHECK(R.gpr[3], 0x11223344);
    CHECK(R.gpr[5], 0x3344);
    CHECK(R.gpr[7], 0x33);

    /* Unaligned, big-endian, sign extended */
    fresh();
    n64_sp_mem[0x11] = 0x80; n64_sp_mem[0x12] = 0x01; n64_sp_mem[0x13] = 0xFF; n64_sp_mem[0x14] = 0x7F;
    RUN(ADDIU(1, 0, 0x11), LW(2, 1, 0), LH(3, 1, 0), LB(4, 1, 0), LBU(5, 1, 0), SB(1, 0, 0x40));
    CHECK(R.gpr[2], 0x8001FF7F);
    CHECK(R.gpr[3], 0xFFFF8001);
    CHECK(R.gpr[4], 0xFFFFFF80);
    CHECK(R.gpr[5], 0x80);
    CHECK(n64_sp_mem[0x40], 0x11);
}

static void test_lqv_sqv(void)
{
    fresh();
    for (int i = 0; i < 64; i++) n64_sp_mem[i] = (u8)(i + 1);
    RUN(LVEC(F_Q, 1, 0, 0, 0),         /* aligned: 16 bytes */
        ADDIU(2, 0, 0x13),
        LVEC(F_Q, 2, 0, 0, 2),         /* 0x13: stops at 0x20 */
        LVEC(F_R, 3, 0, 0, 2),         /* LRV 0x13: 3 bytes at the end */
        SVEC(F_Q, 1, 0, 2, 0),         /* aligned store at 0x20 */
        ADDIU(3, 0, 0x45),
        SVEC(F_Q, 1, 0, 0, 3));        /* unaligned: 0x45..0x4F */
    CHECKV(R.vr[1], 0x0102, 0x0304, 0x0506, 0x0708, 0x090A, 0x0B0C, 0x0D0E, 0x0F10);
    CHECKV(R.vr[2], 0x1415, 0x1617, 0x1819, 0x1A1B, 0x1C1D, 0x1E1F, 0x2000, 0x0000);
    CHECKV(R.vr[3], 0, 0, 0, 0, 0, 0, 0x0011, 0x1213);
    CHECK(n64_sp_mem[0x20], 1);
    CHECK(n64_sp_mem[0x2F], 16);
    CHECK(n64_sp_mem[0x44], 0);
    CHECK(n64_sp_mem[0x45], 1);
    CHECK(n64_sp_mem[0x4F], 11);
    CHECK(n64_sp_mem[0x50], 0);

    /* LQV with an element and at the end of DMEM */
    fresh();
    for (int i = 0; i < 16; i++) n64_sp_mem[0xFF0 + i] = (u8)(0xA0 + i);
    n64_sp_mem[0] = 0x55;
    RUN(ADDIU(1, 0, 0xFF0), LVEC(F_Q, 1, 4, 0, 1));
    CHECKV(R.vr[1], 0, 0, 0xA0A1, 0xA2A3, 0xA4A5, 0xA6A7, 0xA8A9, 0xAAAB);

    /* LDV past the register end does not wrap; MFC2/MTC2 elements */
    fresh();
    for (int i = 0; i < 16; i++) n64_sp_mem[0x100 + i] = (u8)(i + 1);
    setall(1, 0x7777);
    RUN(ADDIU(1, 0, 0x100), LVEC(F_D, 1, 12, 0, 1),
        ADDIU(2, 0, -2), MTC2(2, 2, 15), MTC2(2, 3, 3), MFC2(3, 1, 15), MFC2(4, 2, 15));
    CHECKV(R.vr[1], 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x0102, 0x0304);
    CHECKV(R.vr[2], 0, 0, 0, 0, 0, 0, 0, 0x00FF);   /* element 15: one byte only */
    CHECKV(R.vr[3], 0, 0x00FF, 0xFE00, 0, 0, 0, 0, 0);
    CHECK(R.gpr[3], 0x0477);                       /* element 15 wraps to byte 0 */
    CHECK(R.gpr[4], 0xFFFFFF00);

    /* Packed loads and stores */
    fresh();
    for (int i = 0; i < 16; i++) n64_sp_mem[0x200 + i] = (u8)(0x80 + i);
    RUN(ADDIU(1, 0, 0x200), LVEC(F_P, 1, 0, 0, 1), LVEC(F_U, 2, 0, 0, 1), LVEC(F_H, 3, 0, 0, 1),
        SVEC(F_U, 2, 0, 8, 1), SVEC(F_P, 1, 0, 9, 1), SVEC(F_H, 3, 0, 8, 1));
    CHECKV(R.vr[1], 0x8000, 0x8100, 0x8200, 0x8300, 0x8400, 0x8500, 0x8600, 0x8700);
    CHECKV(R.vr[2], 0x4000, 0x4080, 0x4100, 0x4180, 0x4200, 0x4280, 0x4300, 0x4380);
    CHECKV(R.vr[3], 0x4000, 0x4100, 0x4200, 0x4300, 0x4400, 0x4500, 0x4600, 0x4700);
    CHECK(n64_sp_mem[0x240], 0x80);   /* SUV gives back the bytes */
    CHECK(n64_sp_mem[0x247], 0x87);
    CHECK(n64_sp_mem[0x248], 0x80);   /* SPV takes the high byte */
    CHECK(n64_sp_mem[0x24F], 0x87);
    CHECK(n64_sp_mem[0x280], 0x80);   /* SHV: every other byte */
    CHECK(n64_sp_mem[0x282], 0x82);

    /* LSV/LLV, then fourths and wrapped stores */
    fresh();
    for (int i = 0; i < 16; i++) n64_sp_mem[0x400 + i] = (u8)(0x80 + i);
    RUN(ADDIU(1, 0, 0x400), LVEC(F_S, 4, 2, 0, 1), LVEC(F_L, 5, 4, 1, 1), LVEC(F_B, 5, 1, 2, 1),
        LVEC(F_F, 1, 0, 0, 1), SVEC(F_F, 1, 0, 2, 1), SVEC(F_W, 5, 2, 4, 1));
    CHECKV(R.vr[4], 0, 0x8081, 0, 0, 0, 0, 0, 0);
    CHECKV(R.vr[5], 0x0082, 0, 0x8485, 0x8687, 0, 0, 0, 0);
    CHECKV(R.vr[1], 0x4000, 0x4200, 0x4400, 0x4600, 0, 0, 0, 0);   /* 8 bytes from e */
    CHECK(n64_sp_mem[0x420], 0x80);
    CHECK(n64_sp_mem[0x424], 0x84);
    CHECK(n64_sp_mem[0x42C], 0x8C);
    CHECK(n64_sp_mem[0x441], 0x00);   /* SWV from element 2: byte 3 */
    CHECK(n64_sp_mem[0x442], 0x84);   /* byte 4 */
    CHECK(n64_sp_mem[0x44E], 0x00);   /* wraps to byte 0 */
    CHECK(n64_sp_mem[0x44F], 0x82);   /* byte 1 */

    /* Transposes */
    fresh();
    for (int i = 0; i < 16; i++) n64_sp_mem[0x300 + i] = (u8)i;
    RUN(ADDIU(1, 0, 0x300), LVEC(F_T, 8, 0, 0, 1), SVEC(F_T, 8, 0, 1, 1));
    CHECK(R.vr[8][0], 0x0001);
    CHECK(R.vr[9][1], 0x0203);
    CHECK(R.vr[15][7], 0x0E0F);
    for (int i = 0; i < 16; i++) CHECK(n64_sp_mem[0x310 + i], i);
}

static void test_mul(void)
{
    fresh();
    setv(1, 0x4000, 0x8000, 0x7FFF, 0xFFFF, 0x0001, 0x8000, 0x0000, 0x0100);
    setv(2, 0x4000, 0x8000, 0x7FFF, 0x0001, 0x4000, 0x7FFF, 0x1234, 0x0100);
    RUN(VOP(VMULF, 3, 1, 2, 0));
    CHECKV(R.vr[3], 0x2000, 0x7FFF, 0x7FFE, 0x0000, 0x0001, 0x8001, 0x0000, 0x0002);
    CHECKV(R.accl, 0x8000, 0x8000, 0x8002, 0x7FFE, 0x0000, 0x8000, 0x8000, 0x8000);
    CHECKV(R.accm, 0x2000, 0x8000, 0x7FFE, 0x0000, 0x0001, 0x8001, 0x0000, 0x0002);
    CHECKV(R.acch, 0, 0, 0, 0, 0, 0xFFFF, 0, 0);

    /* VMULU: negative -> 0, 0x8000 result -> FFFF */
    fresh();
    setv(1, 0x8000, 0xFFFF, 0x4000, 0, 0, 0, 0, 0);
    setv(2, 0x8000, 0x0001, 0x4000, 0, 0, 0, 0, 0);
    RUN(VOP(VMULU, 3, 1, 2, 0));
    CHECKV(R.vr[3], 0xFFFF, 0x0000, 0x2000, 0, 0, 0, 0, 0);

    /* VMUDH then VMADH: accumulator keeps growing, output clamps */
    fresh();
    setall(1, 0x4000);
    setall(2, 0x4000);
    RUN(VOP(VMUDH, 3, 1, 2, 0), VOP(VMADH, 4, 1, 2, 0));
    CHECK(R.vr[3][0], 0x7FFF);
    CHECK(R.acch[0], 0x2000);
    CHECK(R.accm[0], 0);
    CHECK(R.accl[0], 0);
    CHECK(R.vr[4][0], 0x7FFF);

    fresh();
    setall(1, 0x7FFF);
    setall(2, 0x8000);
    RUN(VOP(VMUDH, 3, 1, 2, 0), VOP(VMADH, 4, 1, 2, 0), VOP(VMADH, 5, 1, 2, 0));
    CHECK(R.vr[3][0], 0x8000);
    CHECK(R.vr[4][0], 0x8000);
    /* 3 * 0x7FFF * -0x8000 overflows bits 47..16 and wraps positive */
    CHECK(R.vr[5][0], 0x7FFF);
    CHECK(R.acch[0], 0x4001);
    CHECK(R.accm[0], 0x8000);

    /* small positive and negative pass through */
    fresh();
    setall(1, 0x0002);
    setall(2, 0xFFFD);
    RUN(VOP(VMUDH, 3, 1, 2, 0), VOP(VSAR, 4, 0, 0, 8), VOP(VSAR, 5, 0, 0, 9), VOP(VSAR, 6, 0, 0, 10),
        VOP(VSAR, 7, 0, 0, 0));
    CHECK(R.vr[3][0], 0xFFFA);
    CHECK(R.vr[4][0], 0xFFFF);
    CHECK(R.vr[5][0], 0xFFFA);
    CHECK(R.vr[6][0], 0);
    CHECK(R.vr[7][0], 0);

    /* VMUDL unsigned * unsigned, VMUDM signed * unsigned, VMUDN unsigned * signed */
    fresh();
    setall(1, 0xFFFF);
    setall(2, 0xFFFF);
    setall(8, 0x0004);
    RUN(VOP(VMUDL, 3, 1, 8, 0), VOP(VMUDM, 4, 1, 2, 0), VOP(VMUDN, 5, 1, 2, 0));
    /* VMUDL keeps the high half of the unsigned product: 0x3FFFC >> 16 */
    CHECK(R.vr[3][0], 0x0003);
    /* VMUDM: -1 * 0xFFFF = -0xFFFF: mid FFFF */
    CHECK(R.vr[4][0], 0xFFFF);
    /* VMUDN: 0xFFFF * -1 = -0xFFFF: low 0001 */
    CHECK(R.vr[5][0], 0x0001);
    CHECK(R.acch[0], 0xFFFF);
    CHECK(R.accm[0], 0xFFFF);
    CHECK(R.accl[0], 0x0001);

    /* VMUDL: 0xFFFE0001 >> 16 lands in the low accumulator slice only */
    fresh();
    setall(1, 0xFFFF);
    setall(2, 0xFFFF);
    RUN(VOP(VMUDL, 3, 1, 2, 0));
    CHECK(R.vr[3][0], 0xFFFE);
    CHECK(R.accm[0], 0x0000);
    CHECK(R.accl[0], 0xFFFE);

    /* 32-bit fixed point multiply chain: VMUDL VMADM VMADN VMADH.
     * 0x00018000 (1.5) * 0x00020000 (2.0) as 16.16 = 3.0 */
    fresh();
    setall(1, 0x0001); setall(2, 0x8000);    /* a int, frac */
    setall(3, 0x0002); setall(4, 0x0000);    /* b int, frac */
    RUN(VOP(VMUDL, 10, 2, 4, 0), VOP(VMADM, 11, 1, 4, 0), VOP(VMADN, 12, 2, 3, 0), VOP(VMADH, 13, 1, 3, 0));
    CHECK(R.vr[12][0], 0x0000);   /* result fraction */
    CHECK(R.vr[13][0], 0x0003);   /* result integer */

    /* VMACF adds 2p, VMACU clamps unsigned */
    fresh();
    setall(1, 0x4000);
    setall(2, 0x4000);
    RUN(VOP(VMULF, 3, 1, 2, 0), VOP(VMACF, 4, 1, 2, 0), VOP(VMACU, 5, 1, 2, 0), VOP(VMACF, 6, 1, 2, 0));
    CHECK(R.vr[4][0], 0x4000);
    CHECK(R.vr[5][0], 0x6000);
    CHECK(R.vr[6][0], 0x7FFF);    /* 0x8000 clamps */
    CHECK(R.accm[0], 0x8000);
    CHECK(R.accl[0], 0x8000);

    /* VMACU clamps a negative accumulator to 0 */
    fresh();
    setall(1, 0x8000);
    setall(2, 0x7FFF);
    RUN(VOP(VMACU, 3, 1, 2, 0));
    CHECK(R.vr[3][0], 0);

    /* Low-slice clamp in VMADN once the accumulator is large */
    fresh();
    setall(1, 0x7FFF);
    setall(2, 0x7FFF);
    setall(3, 0x0010);
    RUN(VOP(VMUDH, 4, 1, 2, 0), VOP(VMADN, 5, 3, 0, 0));
    CHECK(R.vr[5][0], 0xFFFF);
    fresh();
    setall(1, 0x7FFF);
    setall(2, 0x8000);
    RUN(VOP(VMUDH, 4, 1, 2, 0), VOP(VMADN, 5, 0, 0, 0));
    CHECK(R.vr[5][0], 0);

    /* Carry from low into mid across the 48-bit accumulator wrap */
    fresh();
    for (int i = 0; i < 8; i++) { R.acch[i] = 0xFFFF; R.accm[i] = 0xFFFF; R.accl[i] = 0xFFFF; }
    setall(1, 0xFFFF);
    setall(2, 0xFFFF);
    RUN(VOP(VMADL, 3, 1, 2, 0));
    CHECK(R.acch[0], 0);
    CHECK(R.accm[0], 0);
    CHECK(R.accl[0], 0xFFFD);
}

static void test_element_modes(void)
{
    fresh();
    setv(1, 1, 2, 3, 4, 5, 6, 7, 8);
    setall(2, 0x0100);
    RUN(VOP(VMUDH, 3, 2, 1, 2), VOP(VMUDH, 4, 2, 1, 3), VOP(VMUDH, 5, 2, 1, 5),
        VOP(VMUDH, 6, 2, 1, 15), VOP(VMOV, 7, 3, 1, 9));
    CHECKV(R.vr[3], 0x0100, 0x0100, 0x0300, 0x0300, 0x0500, 0x0500, 0x0700, 0x0700);
    CHECKV(R.vr[4], 0x0200, 0x0200, 0x0400, 0x0400, 0x0600, 0x0600, 0x0800, 0x0800);
    CHECKV(R.vr[5], 0x0200, 0x0200, 0x0200, 0x0200, 0x0600, 0x0600, 0x0600, 0x0600);
    CHECKV(R.vr[6], 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800);
    CHECKV(R.vr[7], 0, 0, 0, 2, 0, 0, 0, 0);
    CHECKV(R.accl, 2, 2, 2, 2, 2, 2, 2, 2);
}

static void test_add_sub(void)
{
    fresh();
    setv(1, 0x7FFF, 0x8000, 1, 0xFFFF, 0x7000, 0x8000, 5, 0);
    setv(2, 0x0001, 0xFFFF, 1, 0x0001, 0x7000, 0x8000, 3, 0);
    RUN(ADDIU(1, 0, 0x0005), CTC2(1, 0), VOP(VADD, 3, 1, 2, 0), CFC2(2, 0), VOP(VSAR, 5, 0, 0, 10),
        ADDIU(1, 0, 0x0102), CTC2(1, 0), VOP(VSUB, 4, 1, 2, 0));
    /* carry-in on elements 0 and 2 */
    CHECKV(R.vr[3], 0x7FFF, 0x8000, 3, 0, 0x7FFF, 0x8000, 8, 0);
    CHECK(R.vr[5][0], 0x8001);
    CHECK(R.vr[5][4], 0xE000);
    CHECK(R.gpr[2], 0);
    /* VSUB borrow on element 1 */
    CHECKV(R.vr[4], 0x7FFE, 0x8000, 0, 0xFFFE, 0, 0, 2, 0);

    /* VADDC/VSUBC flags */
    fresh();
    setv(1, 0xFFFF, 0x0001, 0x8000, 0, 5, 5, 0, 1);
    setv(2, 0x0001, 0x0002, 0x8000, 0, 5, 6, 1, 0);
    RUN(VOP(VADDC, 3, 1, 2, 0), CFC2(1, 0), VOP(VSUBC, 4, 1, 2, 0), CFC2(2, 0));
    CHECKV(R.vr[3], 0, 3, 0, 0, 10, 11, 1, 1);
    CHECK(R.gpr[1], 0x0005);
    CHECKV(R.vr[4], 0xFFFE, 0xFFFF, 0, 0, 0, 0xFFFF, 0xFFFF, 1);
    /* elements differing: 0,1,5,6,7. borrow: 1,5,6 */
    CHECK(R.gpr[2], 0xFFFFE362);

    /* VCO round trip sign extends */
    fresh();
    RUN(LUI(1, 0), ORI(1, 1, 0x8001), CTC2(1, 1), CFC2(2, 1), CTC2(1, 2), CFC2(3, 2));
    CHECK(R.gpr[2], 0xFFFF8001);
    CHECK(R.gpr[3], 0x0001);

    /* VABS */
    fresh();
    setv(1, 0xFFFF, 0, 1, 0x8000, 0xFFFF, 1, 0xFFFF, 0);
    setv(2, 0x0005, 9, 7, 0x8000, 0xFFFD, 0xFFF0, 0, 0);
    RUN(VOP(VABS, 3, 1, 2, 0));
    CHECKV(R.vr[3], 0xFFFB, 0, 7, 0x7FFF, 3, 0xFFF0, 0, 0);
    CHECK(R.accl[3], 0x8000);

    /* logic */
    fresh();
    setall(1, 0xF0F0);
    setall(2, 0xFF00);
    RUN(VOP(VAND, 3, 1, 2, 0), VOP(VNAND, 4, 1, 2, 0), VOP(VOR, 5, 1, 2, 0), VOP(VNOR, 6, 1, 2, 0),
        VOP(VXOR, 7, 1, 2, 0), VOP(VNXOR, 8, 1, 2, 0));
    CHECK(R.vr[3][0], 0xF000);
    CHECK(R.vr[4][0], 0x0FFF);
    CHECK(R.vr[5][0], 0xFFF0);
    CHECK(R.vr[6][0], 0x000F);
    CHECK(R.vr[7][0], 0x0FF0);
    CHECK(R.vr[8][0], 0xF00F);
    CHECK(R.accl[0], 0xF00F);
}

static void test_divide(void)
{
    /* ROM spot checks, from the published table */
    CHECK(rcp_rom[0], 0xFFFF);
    CHECK(rcp_rom[1], 0xFF00);
    CHECK(rcp_rom[2], 0xFE01);
    CHECK(rcp_rom[3], 0xFD04);
    CHECK(rcp_rom[511], 0x0040);
    CHECK(rsq_rom[0], 0x6A09);
    CHECK(rsq_rom[1], 0xFFFF);
    CHECK(rsq_rom[2], 0x6955);
    CHECK(rsq_rom[3], 0xFF00);
    CHECK(rsq_rom[4], 0x68A1);
    CHECK(rsq_rom[5], 0xFE02);

    fresh();
    setv(1, 1, 2, 0, 0x8000, 0xFFFF, 0x0100, 3, 0x7FFF);
    /* VRCP vd[de], vt[e]; follow with VRCPH to read the high half */
    RUN(VOP(VRCP, 2, 0, 1, 8), VOP(VRCPH, 3, 0, 1, 8));
    CHECK(R.vr[2][0], 0xC000);
    CHECK(R.vr[3][0], 0x7FFF);

    fresh();
    setv(1, 1, 2, 0, 0x8000, 0xFFFF, 0x0100, 3, 0x7FFF);
    RUN(VOP(VRCP, 2, 1, 1, 9), VOP(VRCPH, 3, 1, 1, 9));   /* 1/2 */
    CHECK(R.vr[2][1], 0xE000);
    CHECK(R.vr[3][1], 0x3FFF);

    /* 1/0 and 1/-32768 */
    fresh();
    setv(1, 1, 2, 0, 0x8000, 0xFFFF, 0x0100, 3, 0x7FFF);
    RUN(VOP(VRCP, 2, 2, 1, 10), VOP(VRCPH, 3, 2, 1, 10), VOP(VRCP, 4, 3, 1, 11), VOP(VRCPH, 5, 3, 1, 11));
    CHECK(R.vr[2][2], 0xFFFF);
    CHECK(R.vr[3][2], 0x7FFF);
    CHECK(R.vr[4][3], 0x0000);
    CHECK(R.vr[5][3], 0xFFFF);

    /* Negative: 1/-1 */
    fresh();
    setv(1, 1, 2, 0, 0x8000, 0xFFFF, 0x0100, 3, 0x7FFF);
    RUN(VOP(VRCP, 2, 4, 1, 12), VOP(VRCPH, 3, 4, 1, 12));
    CHECK(R.vr[2][4], 0x3FFF);
    CHECK(R.vr[3][4], 0x8000);

    /* Double precision: VRCPH loads the top half, VRCPL divides.
     * 1/0x00020000 */
    fresh();
    setv(1, 0x0002, 0x0000, 0, 0, 0, 0, 0, 0);
    RUN(VOP(VRCPH, 2, 0, 1, 8), VOP(VRCPL, 3, 1, 1, 9), VOP(VRCPH, 4, 2, 1, 10));
    CHECK(R.div_dp, 1);
    CHECK(R.vr[3][1], 0x3FFF);   /* 0x7FFFC000 >> 17 */
    CHECK(R.vr[4][2], 0x0000);

    /* VRCPL without the high half acts on the sign-extended low */
    fresh();
    setv(1, 2, 0, 0, 0, 0, 0, 0, 0);
    RUN(VOP(VRCPL, 3, 0, 1, 8), VOP(VRCPH, 4, 0, 1, 8));
    CHECK(R.vr[3][0], 0xE000);
    CHECK(R.vr[4][0], 0x3FFF);

    /* VRSQ: 1/sqrt(1), 1/sqrt(2), 1/sqrt(4) */
    fresh();
    setv(1, 1, 2, 4, 0, 0, 0, 0, 0);
    RUN(VOP(VRSQ, 2, 0, 1, 8), VOP(VRSQH, 3, 0, 1, 8),
        VOP(VRSQ, 2, 1, 1, 9), VOP(VRSQH, 3, 1, 1, 9),
        VOP(VRSQ, 2, 2, 1, 10), VOP(VRSQH, 3, 2, 1, 10));
    CHECK(R.vr[2][0], 0xC000);
    CHECK(R.vr[3][0], 0x7FFF);
    CHECK(R.vr[3][1], 0x5A82);
    CHECK(R.vr[3][2], 0x3FFF);
    CHECKV(R.accl, 4, 4, 4, 4, 4, 4, 4, 4);

    /* Double-precision VRSQL with no prior VRSQH path */
    fresh();
    setv(1, 0, 4, 0, 0, 0, 0, 0, 0);
    RUN(VOP(VRSQH, 2, 0, 1, 8), VOP(VRSQL, 3, 1, 1, 9), VOP(VRSQH, 4, 1, 1, 9));
    CHECK(R.vr[4][1], 0x3FFF);
}

static void test_compare(void)
{
    fresh();
    setv(1, 1, 2, 3, 0xFFFF, 5, 5, 0x8000, 7);
    setv(2, 2, 2, 2, 0x0001, 5, 5, 0x7FFF, 7);
    /* VCO: element 5 has both carry and ne, element 4 only ne, 7 only carry */
    RUN(ORI(1, 0, 0x3080 | 0x20), CTC2(1, 0), VOP(VLT, 3, 1, 2, 0), CFC2(2, 1), CFC2(3, 0));
    /* lt on 0, 3, 6; equal with both flags on 5 */
    CHECK(R.gpr[2], 0x69);
    CHECK(R.gpr[3], 0);
    CHECKV(R.vr[3], 1, 2, 2, 0xFFFF, 5, 5, 0x8000, 7);

    fresh();
    setv(1, 1, 2, 3, 0xFFFF, 5, 5, 0x8000, 7);
    setv(2, 2, 2, 2, 0x0001, 5, 5, 0x7FFF, 7);
    RUN(ORI(1, 0, 0x30A0), CTC2(1, 0), VOP(VEQ, 3, 1, 2, 0), CFC2(2, 1),
        ORI(1, 0, 0x30A0), CTC2(1, 0), VOP(VNE, 4, 1, 2, 0), CFC2(3, 1),
        ORI(1, 0, 0x30A0), CTC2(1, 0), VOP(VGE, 5, 1, 2, 0), CFC2(4, 1));
    CHECK(R.gpr[2], 0x82);   /* eq on 1, 7; 4,5 have ne set */
    CHECK(R.gpr[3], 0x7D);   /* all but 1 and 7 */
    CHECK(R.gpr[4], 0x96);   /* gt on 2; eq on 1,4,7 (5 has both flags) */

    /* VMRG follows VCC low, clears VCO */
    fresh();
    setall(1, 0xAAAA);
    setall(2, 0x5555);
    RUN(ORI(1, 0, 0xFF), CTC2(1, 0), ORI(1, 0, 0x0F), CTC2(1, 1), VOP(VMRG, 3, 1, 2, 0), CFC2(2, 0));
    CHECKV(R.vr[3], 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0x5555, 0x5555, 0x5555, 0x5555);
    CHECK(R.gpr[2], 0);

    /* VCH, same signs */
    fresh();
    setv(1, 100, 100, 0xFF9C, 0xFF9C, 50, 0x7000, 0xFFFF, 0);
    setv(2, 50,  200, 0xFFCE, 0xFF00, 50, 0x7000, 0xFFFF, 0);
    RUN(VOP(VCH, 3, 1, 2, 0), CFC2(1, 0), CFC2(2, 1), CFC2(3, 2));
    CHECKV(R.vr[3], 50, 100, 0xFF9C, 0xFF00, 50, 0x7000, 0xFFFF, 0);
    CHECK(R.gpr[1], 0x0F00);                 /* ne on 0-3, no sign-difference */
    /* VCC low: vte < 0 on 2,3,6. high: vs >= vte on 0,3,4,5,6,7 */
    CHECK(R.gpr[2], 0xFFFFF900u | 0x4C);
    CHECK(R.gpr[3], 0);

    /* VCH, opposite signs */
    fresh();
    setv(1, 100, 100, 0xFF9C, 0xFF9C, 0xFFFF, 0xFFFE, 100, 100);
    setv(2, 0xFF9C, 0xFF00, 100, 200, 0, 1, 0xFF9C, 0xFF9B);
    RUN(VOP(VCH, 3, 1, 2, 0), CFC2(1, 0), CFC2(2, 1), CFC2(3, 2));
    /* sums: 0, -156, 0, 100, -1, -1, 0, -1 */
    CHECKV(R.vr[3], 100, 0x0100, 0xFF9C, 0xFF9C, 0, 0xFFFF, 100, 101);
    /* VCO low all set; high (sum != 0 and s != ~t): 1, 3 only (5: 0xFFFE=~1) */
    CHECK(R.gpr[1], 0x0AFF);
    /* VCCL (sum <= 0): 0,1,2,4,5,6,7. VCCH (vt < 0): 0,1,6,7 */
    CHECK(R.gpr[2], 0xFFFFC3F7u);
    /* VCE (sum == -1): 4,5,7 */
    CHECK(R.gpr[3], 0xB0);

    /* VCL following VCH: 32-bit clip of (hi:lo) against +/-w */
    fresh();
    /* Every path, with the VCO state set up by CTC2 */
    setv(1, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0xFFFF, 0x0001);
    setv(2, 0x2000, 0x0800, 0xF000, 0xF000, 0xFFFF, 0x0800, 0x0001, 0xFFFF);
    /* VCOL=0xFC (elements 2-7), VCOH=0x10|0x20 hi nibble: elements 4,5 */
    /* VCC low on 4; VCC high on 5. VCE on 3 and 7 */
    RUN(ORI(1, 0, 0x30FC), CTC2(1, 0), ORI(1, 0, 0x2010), CTC2(1, 1), ORI(1, 0, 0x88), CTC2(1, 2),
        VOP(VCL, 3, 1, 2, 0), CFC2(2, 0), CFC2(3, 1), CFC2(4, 2));
    /*  0: !co !ne: s>=t? no, VCCH=0 -> s
     *  1: !co !ne: yes, VCCH=1 -> t
     *  2: co !ne, !vce: sum 0 with carry -> (zero && !carry)=0 -> s
     *  3: co !ne, vce: zero -> 1 -> -t = 0x1000
     *  4: co ne: VCCL kept 1 -> -t = 1
     *  5: co ne: VCCL 0 -> s
     *  6: co !ne, !vce: sum 0 carry -> 0 -> s
     *  7: co !ne, vce: sum 0 carry, zero -> 1 -> -t = 1
     */
    CHECKV(R.vr[3], 0x1000, 0x0800, 0x1000, 0x1000, 0x0001, 0x1000, 0xFFFF, 0x0001);
    CHECK(R.gpr[2], 0);
    CHECK(R.gpr[4], 0);
    /* VCCL: 3,4,7 ; VCCH: 1, 5 (kept) , 0 cleared */
    CHECK(R.gpr[3], 0x2298);

    /* VCL no-zero path: sum has no carry and is nonzero */
    fresh();
    setall(1, 0x0010);
    setall(2, 0x0020);
    RUN(ORI(1, 0, 0x00FF), CTC2(1, 0), VOP(VCL, 3, 1, 2, 0), CFC2(2, 1));
    CHECK(R.vr[3][0], 0x0010);
    CHECK(R.gpr[2], 0);

    /* VCR */
    fresh();
    setv(1, 100, 100, 0xFF9C, 0xFF9C, 0, 0, 0, 0);
    setv(2, 50, 200, 50, 0xFF00, 0, 0, 0, 0);
    RUN(VOP(VCR, 3, 1, 2, 0), CFC2(2, 1));
    /* 0: same sign, 100>=50 -> t, high. 1: 100<200 -> s.
     * 2: differ, -100+50+1<=0 -> ~t, low. 3: same sign, -100>=-256 -> t, low (t<0), high.
     * 4-7: 0>=0 -> t, high */
    CHECKV(R.vr[3], 50, 100, 0xFFCD, 0xFF00, 0, 0, 0, 0);
    CHECK(R.gpr[2], 0xFFFFF90Cu);
}

static void bench(void)
{
    fresh();
    /* A vertex-transform-like loop of mostly vector instructions */
    u32 code[] = {
        LVEC(F_Q, 1, 0, 0, 0), LVEC(F_Q, 2, 0, 2, 0),
        VOP(VMUDH, 3, 1, 2, 8), VOP(VMADL, 3, 1, 2, 9), VOP(VMADM, 3, 2, 1, 10), VOP(VMADN, 4, 2, 1, 11),
        VOP(VMADH, 5, 1, 1, 8), VOP(VSAR, 6, 0, 0, 9), VOP(VADD, 7, 3, 4, 0), VOP(VSUB, 8, 3, 4, 0),
        VOP(VCH, 9, 7, 8, 0), VOP(VCL, 9, 7, 8, 0), VOP(VMULF, 10, 3, 4, 0), VOP(VMACF, 10, 3, 4, 0),
        SVEC(F_Q, 10, 0, 4, 0), ADDIU(1, 1, 1), J(0), NOP,
    };
    load_at(0, code, (int)(sizeof code / sizeof code[0]));
    for (int i = 0; i < 64; i++) n64_sp_mem[i] = (u8)(i * 37);
    rsp_set_pc(0);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    long total = 0;
    for (int i = 0; i < 40; i++) total += rsp_run(1 << 24);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double sec = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) * 1e-9;
    printf("bench: %.0f M instructions/s (real RSP 62.5)\n", (double)total / sec / 1e6);
}

int main(int argc, char **argv)
{
    test_scalar();
    test_branches();
    test_halt_cop0();
    test_dmem();
    test_lqv_sqv();
    test_mul();
    test_element_modes();
    test_add_sub();
    test_divide();
    test_compare();
    printf("%d checks, %d failures\n", checks, failures);
    (void)argv;
    if (argc < 2) bench();         /* any argument skips the benchmark */
    return failures != 0;
}

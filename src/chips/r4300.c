/*
 * r4300.c - NEC VR4300 (MIPS III) interpreter for the Nintendo 64 backend.
 *
 * Addresses are handled as 32-bit values (the N64 runs in 32-bit addressing
 * mode; KX/SX/UX and the 64-bit segments are not modelled). COP0 registers are
 * held in the public u32 array r4300_cop0, so 64-bit registers such as EPC read
 * back sign-extended.
 *
 * Execution model: `pc` is the next instruction, `npc` the one after it, so a
 * branch only has to overwrite npc. `used >= s_limit` is the single per
 * instruction event test: anything that needs attention (pending interrupt,
 * Count/Compare reprogrammed) sets s_limit to 0 and the loop re-evaluates.
 * Count is advanced lazily by count_sync(); s_limit is always clamped to the
 * moment Count reaches Compare.
 *
 * Instructions are fetched through a one page cache, data through direct KSEG
 * decoding or, for mapped addresses, a 4 KB direct mapped cache in front of
 * the 32-entry TLB. Neither stores host pointers, so a save state is simply
 * the N64_STATE section.
 *
 * Rounding: the host rounding mode follows FCR31.RM. It is only changed when
 * the guest's mode differs, and is restored to nearest when r4300_run returns,
 * so other chips never see a guest rounding mode.
 */
#include "n64.h"
#include <fenv.h>
#include <math.h>
#include <string.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

enum {
    C0_INDEX = 0, C0_RANDOM = 1, C0_ENTRYLO0 = 2, C0_ENTRYLO1 = 3, C0_CONTEXT = 4,
    C0_PAGEMASK = 5, C0_WIRED = 6, C0_BADVADDR = 8, C0_COUNT = 9, C0_ENTRYHI = 10,
    C0_COMPARE = 11, C0_STATUS = 12, C0_CAUSE = 13, C0_EPC = 14, C0_PRID = 15,
    C0_CONFIG = 16, C0_LLADDR = 17, C0_WATCHLO = 18, C0_WATCHHI = 19,
    C0_XCONTEXT = 20, C0_PARITY = 26, C0_CACHEERR = 27, C0_TAGLO = 28,
    C0_TAGHI = 29, C0_ERROREPC = 30
};

#define ST_IE   0x00000001u
#define ST_EXL  0x00000002u
#define ST_ERL  0x00000004u
#define ST_BEV  0x00400000u
#define ST_FR   0x04000000u
#define ST_CU0  0x10000000u
#define ST_CU1  0x20000000u
#define ST_CU2  0x40000000u
#define CAUSE_IP2 0x00000400u
#define CAUSE_IP7 0x00008000u
#define CAUSE_BD  0x80000000u

enum {
    EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5,
    EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13,
    EXC_FPE = 15
};
enum { XL_OK = 0, XL_REFILL = 1, XL_INVALID = 2, XL_MODIFIED = 3 };

/* extra cycles beyond the base one (VR4300 result latencies) */
enum {
    COST_MEM = 1, COST_MULT = 4, COST_DMULT = 7, COST_DIV = 36, COST_DDIV = 68,
    COST_FADD = 2, COST_FMUL_S = 4, COST_FMUL_D = 7, COST_FDIV_S = 28,
    COST_FDIV_D = 57, COST_FCVT = 3
};

#define FC_I 0x01000u
#define FC_U 0x02000u
#define FC_O 0x04000u
#define FC_Z 0x08000u
#define FC_V 0x10000u
#define FC_E 0x20000u
#define FC_ALL 0x3F000u
#define FCR_EN_U (1u << 8)
#define FCR_COND (1u << 23)
#define FCR_FS   (1u << 24)
#define FCR31_MASK 0x0183FFFFu

/* ---------------------------------------------------------------- state */
typedef struct { u32 mask, hi, lo0, lo1, present; } TlbEntry;
typedef struct { u32 tag, pa; } TcEntry;

#define TC_SIZE 256u
#define TC_INVALID 0xFFFFFFFFu   /* low bits set: never equals a page base */
#define FETCH_INVALID 1u         /* odd: always fails the fetch page test */

u64 r4300_gpr[32] N64_STATE;
u32 r4300_cop0[32] N64_STATE;

static u64 s_hi N64_STATE, s_lo N64_STATE;
static u32 s_pc N64_STATE, s_npc N64_STATE;
static int s_delay N64_STATE;          /* instruction at s_pc is in a delay slot */
static u32 s_llbit N64_STATE;
static u32 s_cop0_latch N64_STATE;     /* value read from reserved COP0 registers */
static TlbEntry s_tlb[32] N64_STATE;
static TcEntry s_tc[2][TC_SIZE] N64_STATE;   /* [0] reads, [1] writes */
static u32 s_tc_key N64_STATE;         /* ASID and ERL the cache was built for */

/* FPR words: FR=1 reg n is words 2n/2n+1, FR=0 reg n is one half of the even
 * register. Host is little-endian: even word = low half. */
static u32 s_fpw[64] N64_STATE;
static u32 s_fcr31 N64_STATE;

static u32 s_count_frac N64_STATE;     /* half a Count tick carried over */
static u64 s_total N64_STATE;          /* CPU cycles, drives Random */
static u64 s_random_t0 N64_STATE;      /* s_total when Random was last 31 */
static int s_count_base N64_STATE;     /* `used` value Count is synced to */
static int s_limit N64_STATE;
static int s_irq_pending N64_STATE;

static int s_host_rm;                  /* host side, not emulated state */

/* ------------------------------------------------------------ memory helpers */
static inline u16 rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
static inline u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline void wr16(u8 *p, u16 x) { x = __builtin_bswap16(x); memcpy(p, &x, 2); }
static inline void wr32(u8 *p, u32 x) { x = __builtin_bswap32(x); memcpy(p, &x, 4); }
static inline void wr64(u8 *p, u64 x) { x = __builtin_bswap64(x); memcpy(p, &x, 8); }

static inline u32 mem_r32(u32 pa) { return pa < N64_RDRAM_SIZE ? rd32(n64_rdram + pa) : n64_read32(pa); }
static inline u64 mem_r64(u32 pa) { return pa < N64_RDRAM_SIZE ? rd64(n64_rdram + pa) : n64_read64(pa); }
static inline void mem_w32(u32 pa, u32 v) { if (pa < N64_RDRAM_SIZE) wr32(n64_rdram + pa, v); else n64_write32(pa, v); }
static inline void mem_w64(u32 pa, u64 v) { if (pa < N64_RDRAM_SIZE) wr64(n64_rdram + pa, v); else n64_write64(pa, v); }

/* ---------------------------------------------------------------- interrupts */
static inline void update_irq(void) {
    u32 st = r4300_cop0[C0_STATUS];
    s_irq_pending = (st & r4300_cop0[C0_CAUSE] & 0xFF00u) && (st & 7u) == ST_IE;
    if (s_irq_pending) s_limit = 0;
}

void r4300_set_irq(int asserted) {
    if (asserted) r4300_cop0[C0_CAUSE] |= CAUSE_IP2;
    else          r4300_cop0[C0_CAUSE] &= ~CAUSE_IP2;
    update_irq();
}

/* Advance Count to `used` and detect it passing Compare. */
static void count_sync(int used) {
    int delta = used - s_count_base;
    if (delta <= 0) return;
    s_count_base = used;
    s_total += (u64)delta;
    u32 t = (u32)delta + s_count_frac;
    u32 inc = t >> 1;
    s_count_frac = t & 1;
    u32 old = r4300_cop0[C0_COUNT];
    r4300_cop0[C0_COUNT] = old + inc;
    if (inc && r4300_cop0[C0_COMPARE] - old - 1u < inc) {
        r4300_cop0[C0_CAUSE] |= CAUSE_IP7;
        update_irq();
    }
}

static int next_limit(int used, int target) {
    u32 d = r4300_cop0[C0_COMPARE] - r4300_cop0[C0_COUNT];
    u64 to_compare = (d ? (u64)d * 2 : 1ull << 33) - s_count_frac;
    s64 lim = (s64)used + (s64)to_compare;
    return lim < target ? (int)lim : target;
}

static u32 random_value(void) {
    u32 wired = r4300_cop0[C0_WIRED];
    u32 n = wired > 31 ? 32 : 32 - wired;
    return 31 - (u32)((s_total - s_random_t0) % n);
}

/* ---------------------------------------------------------------- translation */
static void tc_flush(void) { memset(s_tc, 0xFF, sizeof s_tc); }

/* The translation cache depends on the ASID and on ERL (which unmaps KUSEG). */
static void tc_validate(void) {
    u32 key = (r4300_cop0[C0_ENTRYHI] & 0xFFu) | (r4300_cop0[C0_STATUS] & ST_ERL) << 8;
    if (key != s_tc_key) {
        s_tc_key = key;
        tc_flush();
    }
}

/* Slow path for every address outside KSEG0/1. Fills the translation cache. */
static int xlate(u32 va, int wr, u32 *pa) {
    if ((s32)va >= 0 && (r4300_cop0[C0_STATUS] & ST_ERL)) {
        *pa = va;
        return XL_OK;
    }
    u32 asid = r4300_cop0[C0_ENTRYHI] & 0xFFu;
    for (int i = 0; i < 32; i++) {
        const TlbEntry *e = &s_tlb[i];
        if (!e->present || ((va ^ e->hi) & ~e->mask)) continue;
        if (!(e->lo0 & 1) && (e->hi & 0xFFu) != asid) continue;   /* G is in both lo */
        u32 off_mask = e->mask >> 1;
        u32 lo = (va & (off_mask + 1)) ? e->lo1 : e->lo0;
        if (!(lo & 2)) return XL_INVALID;
        if (wr && !(lo & 4)) return XL_MODIFIED;
        u32 base = ((lo >> 6) & 0xFFFFFu) << 12;
        *pa = (base & ~off_mask) | (va & off_mask);
        TcEntry *t = &s_tc[wr][(va >> 12) & (TC_SIZE - 1)];
        t->tag = va & ~0xFFFu;
        t->pa = *pa & ~0xFFFu;
        return XL_OK;
    }
    return XL_REFILL;
}

static void tlb_write(u32 idx) {
    TlbEntry *e = &s_tlb[idx & 31];
    u32 *c = r4300_cop0;
    u32 g = c[C0_ENTRYLO0] & c[C0_ENTRYLO1] & 1u;
    e->mask = (c[C0_PAGEMASK] & 0x01FFE000u) | 0x1FFFu;
    e->hi = c[C0_ENTRYHI] & (~e->mask | 0xFFu);
    e->lo0 = (c[C0_ENTRYLO0] & ~1u) | g;
    e->lo1 = (c[C0_ENTRYLO1] & ~1u) | g;
    e->present = 1;
    tc_flush();
}

static void tlb_read(void) {
    u32 *c = r4300_cop0;
    const TlbEntry *e = &s_tlb[c[C0_INDEX] & 31];
    c[C0_PAGEMASK] = e->mask & 0x01FFE000u;
    c[C0_ENTRYHI] = e->hi;
    c[C0_ENTRYLO0] = e->lo0;
    c[C0_ENTRYLO1] = e->lo1;
    tc_validate();
}

static void tlb_probe(void) {
    u32 *c = r4300_cop0;
    u32 hi = c[C0_ENTRYHI];
    c[C0_INDEX] = 0x80000000u;
    for (u32 i = 0; i < 32; i++) {
        const TlbEntry *e = &s_tlb[i];
        if (!e->present || ((hi ^ e->hi) & ~e->mask & ~0xFFu)) continue;
        if (!(e->lo0 & 1) && ((hi ^ e->hi) & 0xFFu)) continue;
        c[C0_INDEX] = i;
        return;
    }
}

/* ---------------------------------------------------------------- exceptions */
static u32 exception_enter(u32 code, u32 ce, u32 cur, int in_delay, u32 vector) {
    u32 *c = r4300_cop0;
    u32 st = c[C0_STATUS];
    u32 cause = c[C0_CAUSE] & ~0x3000007Cu;
    if (!(st & ST_EXL)) {
        if (in_delay) {
            c[C0_EPC] = cur - 4;
            cause |= CAUSE_BD;
        } else {
            c[C0_EPC] = cur;
            cause &= ~CAUSE_BD;
        }
    }
    c[C0_CAUSE] = cause | code << 2 | ce << 28;
    c[C0_STATUS] = st | ST_EXL;
    update_irq();
    return ((st & ST_BEV) ? 0xBFC00200u : 0x80000000u) + vector;
}

static u32 tlb_fault(int kind, int wr, u32 va, u32 cur, int in_delay) {
    u32 *c = r4300_cop0;
    c[C0_BADVADDR] = va;
    c[C0_CONTEXT] = (c[C0_CONTEXT] & 0xFF800000u) | ((va >> 13) << 4);
    c[C0_XCONTEXT] = (c[C0_XCONTEXT] & 0x80000000u) | ((va >> 13) << 4);
    c[C0_ENTRYHI] = (va & 0xFFFFE000u) | (c[C0_ENTRYHI] & 0xFFu);
    tc_validate();
    u32 vector = (kind == XL_REFILL && !(c[C0_STATUS] & ST_EXL)) ? 0x000u : 0x180u;
    u32 code = kind == XL_MODIFIED ? EXC_MOD : wr ? EXC_TLBS : EXC_TLBL;
    return exception_enter(code, 0, cur, in_delay, vector);
}

/* ---------------------------------------------------------------- COP0 access */
static u32 cop0_read(int reg, int used) {
    u32 *c = r4300_cop0;
    switch (reg) {
    case C0_RANDOM:
        count_sync(used);
        return c[C0_RANDOM] = random_value();
    case C0_COUNT:
        count_sync(used);
        return c[C0_COUNT];
    case 7: case 21: case 22: case 23: case 24: case 25: case 31:
        return s_cop0_latch;
    default:
        return c[reg];
    }
}

static void cop0_write(int reg, u32 v, int used) {
    u32 *c = r4300_cop0;
    s_cop0_latch = v;
    switch (reg) {
    case C0_INDEX:    c[reg] = v & 0x8000003Fu; break;
    case C0_ENTRYLO0:
    case C0_ENTRYLO1: c[reg] = v & 0x03FFFFFFu; break;
    case C0_CONTEXT:  c[reg] = (c[reg] & 0x007FFFFFu) | (v & 0xFF800000u); break;
    case C0_PAGEMASK: c[reg] = v & 0x01FFE000u; break;
    case C0_WIRED:
        count_sync(used);
        c[reg] = v & 0x3Fu;
        c[C0_RANDOM] = 31;
        s_random_t0 = s_total;
        break;
    case C0_COUNT:
        count_sync(used);
        c[reg] = v;
        s_limit = 0;
        break;
    case C0_ENTRYHI:
        c[reg] = v & 0xFFFFE0FFu;
        tc_validate();
        break;
    case C0_COMPARE:
        count_sync(used);
        c[reg] = v;
        c[C0_CAUSE] &= ~CAUSE_IP7;
        update_irq();
        s_limit = 0;
        break;
    case C0_STATUS:
        c[reg] = v & 0xFF57FFFFu;
        tc_validate();
        update_irq();
        break;
    case C0_CAUSE:
        c[reg] = (c[reg] & ~0x300u) | (v & 0x300u);
        update_irq();
        break;
    case C0_EPC: case C0_LLADDR: case C0_WATCHLO: case C0_WATCHHI:
    case C0_ERROREPC:
        c[reg] = v;
        break;
    case C0_CONFIG:   c[reg] = (c[reg] & ~0x0F00800Fu) | (v & 0x0F00800Fu); break;
    case C0_XCONTEXT: c[reg] = (c[reg] & 0x7FFFFFFFu) | (v & 0x80000000u); break;
    case C0_PARITY:   c[reg] = v & 0xFFu; break;
    case C0_TAGLO:    c[reg] = v & 0x0FFFFFC0u; break;
    default: break;   /* read-only, TagHi or reserved */
    }
}

/* ---------------------------------------------------------------- FPU helpers */
static inline u32 fpw_index(int n) {
    return (r4300_cop0[C0_STATUS] & ST_FR) ? (u32)n * 2 : ((u32)n & ~1u) * 2 + ((u32)n & 1u);
}
static inline u32 fpd_index(int n) {
    return (r4300_cop0[C0_STATUS] & ST_FR) ? (u32)n * 2 : ((u32)n & ~1u) * 2;
}
static inline u32 fget_w(int n) { return s_fpw[fpw_index(n)]; }
static inline void fset_w(int n, u32 v) { s_fpw[fpw_index(n)] = v; }
static inline u64 fget_l(int n) {
    u32 i = fpd_index(n);
    return (u64)s_fpw[i + 1] << 32 | s_fpw[i];
}
static inline void fset_l(int n, u64 v) {
    u32 i = fpd_index(n);
    s_fpw[i] = (u32)v;
    s_fpw[i + 1] = (u32)(v >> 32);
}
static inline float fget_s(int n) { u32 w = fget_w(n); float f; memcpy(&f, &w, 4); return f; }
static inline void fset_s(int n, float f) { u32 w; memcpy(&w, &f, 4); fset_w(n, w); }
static inline double fget_d(int n) { u64 w = fget_l(n); double d; memcpy(&d, &w, 8); return d; }
static inline void fset_d(int n, double d) { u64 w; memcpy(&w, &d, 8); fset_l(n, w); }

static void host_rm_apply(int rm) {
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    if (rm != s_host_rm) {
        s_host_rm = rm;
        fesetround(modes[rm]);
    }
}

/* Records cause bits. Returns 1 when the operation must trap instead. */
static int fp_raise(u32 cause) {
    s_fcr31 = (s_fcr31 & ~FC_ALL) | cause;
    u32 enabled = (s_fcr31 & 0xF80u) << 5 | FC_E;
    if (cause & enabled) return 1;
    s_fcr31 |= (cause & 0x1F000u) >> 10;
    return 0;
}

enum { FK_ARITH = 0, FK_DIV = 1 };

/* Results with an all-zero or all-one exponent land here: classify the
 * NaN/infinity/denormal case, canonicalise, and decide whether to trap. */
static int fp_fix_s(float *r, float a, float b, int kind) {
    u32 u;
    memcpy(&u, r, 4);
    u32 man = u & 0x7FFFFFu;
    u32 cause;
    if ((u >> 23 & 0xFF) == 0xFF) {
        if (man) {
            u = 0x7FBFFFFFu;
            memcpy(r, &u, 4);
            cause = FC_V;
        } else if (isinf(a) || isinf(b)) {
            return 0;
        } else {
            cause = (kind == FK_DIV && b == 0.0f) ? FC_Z : (FC_O | FC_I);
        }
    } else {
        if (!man) return 0;
        if (!(s_fcr31 & (FCR_FS | FCR_EN_U))) return fp_raise(FC_E);
        u &= 0x80000000u;
        memcpy(r, &u, 4);
        cause = FC_U | FC_I;
    }
    return fp_raise(cause);
}

static int fp_fix_d(double *r, double a, double b, int kind) {
    u64 u;
    memcpy(&u, r, 8);
    u64 man = u & 0xFFFFFFFFFFFFFull;
    u32 cause;
    if ((u >> 52 & 0x7FF) == 0x7FF) {
        if (man) {
            u = 0x7FF7FFFFFFFFFFFFull;
            memcpy(r, &u, 8);
            cause = FC_V;
        } else if (isinf(a) || isinf(b)) {
            return 0;
        } else {
            cause = (kind == FK_DIV && b == 0.0) ? FC_Z : (FC_O | FC_I);
        }
    } else {
        if (!man) return 0;
        if (!(s_fcr31 & (FCR_FS | FCR_EN_U))) return fp_raise(FC_E);
        u &= 0x8000000000000000ull;
        memcpy(r, &u, 8);
        cause = FC_U | FC_I;
    }
    return fp_raise(cause);
}

static inline int fp_special_s(float r) {
    u32 u;
    memcpy(&u, &r, 4);
    u32 e = (u >> 23) & 0xFF;
    return e == 0 || e == 0xFF;
}
static inline int fp_special_d(double r) {
    u64 u;
    memcpy(&u, &r, 8);
    u32 e = (u32)(u >> 52) & 0x7FF;
    return e == 0 || e == 0x7FF;
}

static inline double fp_round_even(double x) {
#if defined(__aarch64__)
    return __builtin_roundeven(x);
#else
    double f = floor(x), d = x - f;
    if (d > 0.5 || (d == 0.5 && fmod(f, 2.0) != 0.0)) f += 1.0;
    return f;
#endif
}

/* float to integer, exact for all modes. Operands the VR4300 cannot convert
 * (NaN, infinity, out of range) are an unimplemented-operation trap. */
static int fp_to_int(double x, int rm, int to_long, s64 *out) {
    double r;
    switch (rm) {
    case 0:  r = fp_round_even(x); break;
    case 1:  r = trunc(x); break;
    case 2:  r = ceil(x); break;
    default: r = floor(x); break;
    }
    int ok = to_long ? fabs(r) < 9007199254740992.0
                     : (r >= -2147483648.0 && r <= 2147483647.0);
    if (!ok) return fp_raise(FC_E);   /* NaN fails both comparisons */
    if (fp_raise(r != x ? FC_I : 0)) return 1;
    *out = (s64)r;
    return 0;
}

static int fp_compare(u32 cond, int unordered, int equal, int less) {
    if (unordered && (cond & 8) && fp_raise(FC_V)) return -1;
    int c = ((cond & 1) && unordered) || ((cond & 2) && equal) || ((cond & 4) && less);
    s_fcr31 = c ? s_fcr31 | FCR_COND : s_fcr31 & ~FCR_COND;
    return 0;
}

#define FS_ ((int)(inst >> 11) & 31)
#define FT_ ((int)(inst >> 16) & 31)
#define FD_ ((int)(inst >> 6) & 31)

#define FP_RESULT(SUF, a, b, kind, cost)                                      \
    do {                                                                      \
        if (unlikely(fp_special_##SUF(r)) && fp_fix_##SUF(&r, a, b, kind))    \
            return -1;                                                        \
        fset_##SUF(FD_, r);                                                   \
        return cost;                                                          \
    } while (0)

#define FP_TO_INT(rm, to_long)                                                \
    do {                                                                      \
        s64 out = 0;                                                          \
        if (fp_to_int(a, rm, to_long, &out)) return -1;                       \
        if (to_long) fset_l(FD_, (u64)out); else fset_w(FD_, (u32)out);       \
        return COST_FCVT;                                                     \
    } while (0)

/* Each returns extra cycles, or -1 for an FPU exception. */
static int cop1_s(u32 inst) {
    u32 func = inst & 63;
    float a = fget_s(FS_), b = fget_s(FT_), r;
    s_fcr31 &= ~FC_ALL;
    switch (func) {
    case 0: r = a + b; FP_RESULT(s, a, b, FK_ARITH, COST_FADD);
    case 1: r = a - b; FP_RESULT(s, a, b, FK_ARITH, COST_FADD);
    case 2: r = a * b; FP_RESULT(s, a, b, FK_ARITH, COST_FMUL_S);
    case 3: r = a / b; FP_RESULT(s, a, b, FK_DIV, COST_FDIV_S);
    case 4: r = sqrtf(a); FP_RESULT(s, a, a, FK_ARITH, COST_FDIV_S);
    case 5: r = fabsf(a); FP_RESULT(s, a, a, FK_ARITH, 0);
    case 6: fset_w(FD_, fget_w(FS_)); return 0;
    case 7: r = -a; FP_RESULT(s, a, a, FK_ARITH, 0);
    case 8: case 9: case 10: case 11: FP_TO_INT((int)func & 3, 1);
    case 12: case 13: case 14: case 15: FP_TO_INT((int)func & 3, 0);
    case 33: {
        double d = a;
        if (unlikely(fp_special_d(d)) && fp_fix_d(&d, a, a, FK_ARITH)) return -1;
        fset_d(FD_, d);
        return 0;
    }
    case 36: FP_TO_INT((int)(s_fcr31 & 3), 0);
    case 37: FP_TO_INT((int)(s_fcr31 & 3), 1);
    default:
        if (func >= 48)
            return fp_compare(func & 15, isnan(a) || isnan(b), a == b, a < b);
        return fp_raise(FC_E) ? -1 : 0;
    }
}

static int cop1_d(u32 inst) {
    u32 func = inst & 63;
    double a = fget_d(FS_), b = fget_d(FT_), r;
    s_fcr31 &= ~FC_ALL;
    switch (func) {
    case 0: r = a + b; FP_RESULT(d, a, b, FK_ARITH, COST_FADD);
    case 1: r = a - b; FP_RESULT(d, a, b, FK_ARITH, COST_FADD);
    case 2: r = a * b; FP_RESULT(d, a, b, FK_ARITH, COST_FMUL_D);
    case 3: r = a / b; FP_RESULT(d, a, b, FK_DIV, COST_FDIV_D);
    case 4: r = sqrt(a); FP_RESULT(d, a, a, FK_ARITH, COST_FDIV_D);
    case 5: r = fabs(a); FP_RESULT(d, a, a, FK_ARITH, 0);
    case 6: fset_l(FD_, fget_l(FS_)); return 0;
    case 7: r = -a; FP_RESULT(d, a, a, FK_ARITH, 0);
    case 8: case 9: case 10: case 11: FP_TO_INT((int)func & 3, 1);
    case 12: case 13: case 14: case 15: FP_TO_INT((int)func & 3, 0);
    case 32: {
        float f = (float)a, in = isinf(a) ? f : 0.0f;
        if (unlikely(fp_special_s(f)) && fp_fix_s(&f, in, in, FK_ARITH)) return -1;
        fset_s(FD_, f);
        return COST_FCVT;
    }
    case 36: FP_TO_INT((int)(s_fcr31 & 3), 0);
    case 37: FP_TO_INT((int)(s_fcr31 & 3), 1);
    default:
        if (func >= 48)
            return fp_compare(func & 15, isnan(a) || isnan(b), a == b, a < b);
        return fp_raise(FC_E) ? -1 : 0;
    }
}

static int cop1_w(u32 inst) {
    s32 v = (s32)fget_w(FS_);
    s_fcr31 &= ~FC_ALL;
    switch (inst & 63) {
    case 32: {
        float f = (float)v;
        if (fp_raise((double)f != (double)v ? FC_I : 0)) return -1;
        fset_s(FD_, f);
        return COST_FCVT;
    }
    case 33: fset_d(FD_, (double)v); return COST_FCVT;
    default: return fp_raise(FC_E) ? -1 : 0;
    }
}

static int cop1_l(u32 inst) {
    s64 v = (s64)fget_l(FS_);
    s_fcr31 &= ~FC_ALL;
    u32 func = inst & 63;
    /* the VR4300 converts 56-bit values only */
    if ((func != 32 && func != 33) || v >= (1ll << 55) || v < -(1ll << 55))
        return fp_raise(FC_E) ? -1 : 0;
    if (func == 32) fset_s(FD_, (float)v);
    else            fset_d(FD_, (double)v);
    return COST_FCVT;
}

/* ---------------------------------------------------------------- reset */
void r4300_reset(u32 pc) {
    memset(r4300_gpr, 0, sizeof r4300_gpr);
    memset(r4300_cop0, 0, sizeof r4300_cop0);
    memset(s_tlb, 0, sizeof s_tlb);
    memset(s_fpw, 0, sizeof s_fpw);
    tc_flush();
    s_tc_key = 0;
    s_hi = s_lo = 0;
    s_pc = pc;
    s_npc = pc + 4;
    s_delay = 0;
    s_llbit = 0;
    s_cop0_latch = 0;
    s_fcr31 = 0;
    s_count_frac = 0;
    s_total = 0;
    s_random_t0 = 0;
    s_count_base = 0;
    s_limit = 0;
    s_irq_pending = 0;
    r4300_cop0[C0_RANDOM] = 31;
    r4300_cop0[C0_STATUS] = ST_BEV | ST_ERL;
    r4300_cop0[C0_PRID] = 0x00000B22u;
    r4300_cop0[C0_CONFIG] = 0x7006E463u;
    r4300_cop0[C0_CONTEXT] = 0;
    tc_validate();
}

/* ---------------------------------------------------------------- interpreter */
#define RS ((inst >> 21) & 31)
#define RT ((inst >> 16) & 31)
#define RD ((inst >> 11) & 31)
#define SA ((inst >> 6) & 31)
#define SIMM ((u64)(s64)(s16)inst)
#define ZIMM ((u64)(inst & 0xFFFFu))
#define SX32(x) ((u64)(s64)(s32)(x))
#define EA ((u32)gpr[RS] + (u32)(s32)(s16)inst)

#define BRANCH(cond)                                                          \
    do {                                                                      \
        nds = 1;                                                              \
        if (cond) npc = pc + ((u32)(s32)(s16)inst << 2);                      \
    } while (0)

/* Not taken: the delay slot is skipped. */
#define BRANCH_LIKELY(cond)                                                   \
    do {                                                                      \
        if (cond) {                                                           \
            nds = 1;                                                          \
            npc = pc + ((u32)(s32)(s16)inst << 2);                            \
        } else {                                                              \
            pc = npc;                                                         \
            npc += 4;                                                         \
        }                                                                     \
    } while (0)

#define TRANSLATE(va, pa, wr)                                                 \
    do {                                                                      \
        if (likely((u32)((va) - 0x80000000u) < 0x40000000u)) {                \
            (pa) = (va) & 0x1FFFFFFFu;                                        \
        } else {                                                              \
            const TcEntry *t_ = &s_tc[wr][((va) >> 12) & (TC_SIZE - 1)];      \
            if (likely(t_->tag == ((va) & ~0xFFFu))) {                        \
                (pa) = t_->pa | ((va) & 0xFFFu);                              \
            } else {                                                          \
                int k_ = xlate(va, wr, &(pa));                                \
                if (unlikely(k_)) {                                           \
                    fault_kind = k_;                                          \
                    fault_wr = wr;                                            \
                    fault_va = va;                                            \
                    goto ex_tlb;                                              \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)

/* Effective address of an aligned access: bad addresses leave the switch. */
#define ADDR(size, wr)                                                        \
    u32 va = EA, pa;                                                          \
    if (unlikely(va & ((size) - 1))) {                                        \
        fault_va = va;                                                        \
        exc_code = (wr) ? EXC_ADES : EXC_ADEL;                                \
        goto ex_addr;                                                         \
    }                                                                         \
    TRANSLATE(va, pa, wr);                                                    \
    used += COST_MEM

#define LOAD(size, type, dest, ramread, ioread)                               \
    {                                                                         \
        ADDR(size, 0);                                                        \
        type v_ = (type)(likely(pa < N64_RDRAM_SIZE) ? ramread : ioread(pa)); \
        dest;                                                                 \
    }                                                                         \
    break

#define STORE(size, val, ramwrite, iowrite)                                   \
    {                                                                         \
        ADDR(size, 1);                                                        \
        if (likely(pa < N64_RDRAM_SIZE)) ramwrite;                            \
        else iowrite(pa, val);                                                \
    }                                                                         \
    break

#define NEED_CU1                                                              \
    if (unlikely(!(cop0[C0_STATUS] & ST_CU1))) {                              \
        exc_ce = 1;                                                           \
        goto ex_cpu;                                                          \
    }

#define NEED_CU2                                                              \
    do {                                                                      \
        if (!(cop0[C0_STATUS] & ST_CU2)) {                                    \
            exc_ce = 2;                                                       \
            goto ex_cpu;                                                      \
        }                                                                     \
        goto ex_ri;                                                           \
    } while (0)

#define TRAP_IF(cond)                                                         \
    do {                                                                      \
        if (cond) {                                                           \
            exc_code = EXC_TR;                                                \
            goto ex_code;                                                     \
        }                                                                     \
    } while (0)

int r4300_run(int cycles) {
    u64 *const gpr = r4300_gpr;
    u32 *const cop0 = r4300_cop0;
    u8 *const ram = n64_rdram;

    u32 pc = s_pc, npc = s_npc;
    int nds = s_delay;              /* the instruction at pc is in a delay slot */
    int ids = 0;                    /* the one being executed is */
    u32 cur = pc, inst;
    int used = 0;
    const int target = cycles;

    u32 ftag = FETCH_INVALID;       /* page of pc when RAM backed */
    const u8 *fbase = ram;

    u32 exc_code = 0, exc_ce = 0, fault_va = 0;
    int fault_kind = 0, fault_wr = 0;

    s_count_base = 0;
    s_limit = 0;
    host_rm_apply((int)(s_fcr31 & 3));
    tc_validate();
    update_irq();

    for (;;) {
        if (unlikely(used >= s_limit)) {
            count_sync(used);
            if (s_irq_pending) {
                pc = exception_enter(EXC_INT, 0, pc, nds, 0x180);
                npc = pc + 4;
                nds = 0;
                continue;
            }
            if (used >= target) break;
            s_limit = next_limit(used, target);
        }

        {
            u32 off = pc ^ ftag;
            if (likely(!(off & ~0xFFCu))) {
                inst = rd32(fbase + off);
            } else {
                u32 pa;
                cur = pc;
                ids = nds;
                if (unlikely(pc & 3)) {
                    fault_va = pc;
                    exc_code = EXC_ADEL;
                    goto ex_addr;
                }
                TRANSLATE(pc, pa, 0);
                if (likely(pa < N64_RDRAM_SIZE)) {
                    ftag = pc & ~0xFFFu;
                    fbase = ram + (pa & ~0xFFFu);
                    inst = rd32(ram + pa);
                } else {
                    ftag = FETCH_INVALID;
                    inst = n64_read32(pa);
                }
            }
        }

        cur = pc;
        ids = nds;
        nds = 0;
        pc = npc;
        npc += 4;
        used++;
        gpr[0] = 0;

        switch (inst >> 26) {
        case 0: /* SPECIAL */
            switch (inst & 63) {
            case 0:  gpr[RD] = SX32((u32)gpr[RT] << SA); break;
            case 2:  gpr[RD] = SX32((u32)gpr[RT] >> SA); break;
            case 3:  gpr[RD] = SX32((s64)gpr[RT] >> SA); break;
            case 4:  gpr[RD] = SX32((u32)gpr[RT] << (gpr[RS] & 31)); break;
            case 6:  gpr[RD] = SX32((u32)gpr[RT] >> (gpr[RS] & 31)); break;
            case 7:  gpr[RD] = SX32((s64)gpr[RT] >> (gpr[RS] & 31)); break;
            case 8:  npc = (u32)gpr[RS]; nds = 1; break;
            case 9: {
                u32 t = (u32)gpr[RS];
                gpr[RD] = SX32(npc);
                npc = t;
                nds = 1;
                break;
            }
            case 12: exc_code = EXC_SYS; goto ex_code;
            case 13: exc_code = EXC_BP; goto ex_code;
            case 15: break; /* SYNC */
            case 16: gpr[RD] = s_hi; break;
            case 17: s_hi = gpr[RS]; break;
            case 18: gpr[RD] = s_lo; break;
            case 19: s_lo = gpr[RS]; break;
            case 20: gpr[RD] = gpr[RT] << (gpr[RS] & 63); break;
            case 22: gpr[RD] = gpr[RT] >> (gpr[RS] & 63); break;
            case 23: gpr[RD] = (u64)((s64)gpr[RT] >> (gpr[RS] & 63)); break;
            case 24: {
                s64 r = (s64)(s32)gpr[RS] * (s64)(s32)gpr[RT];
                s_lo = SX32((u32)r);
                s_hi = SX32((u32)(r >> 32));
                used += COST_MULT;
                break;
            }
            case 25: {
                u64 r = (u64)(u32)gpr[RS] * (u64)(u32)gpr[RT];
                s_lo = SX32((u32)r);
                s_hi = SX32((u32)(r >> 32));
                used += COST_MULT;
                break;
            }
            case 26: {
                s32 n = (s32)gpr[RS], d = (s32)gpr[RT];
                if (d == 0) {
                    s_lo = n < 0 ? 1 : ~0ull;
                    s_hi = SX32(n);
                } else if (d == -1) {
                    s_lo = SX32(0u - (u32)n);
                    s_hi = 0;
                } else {
                    s_lo = SX32(n / d);
                    s_hi = SX32(n % d);
                }
                used += COST_DIV;
                break;
            }
            case 27: {
                u32 n = (u32)gpr[RS], d = (u32)gpr[RT];
                if (d == 0) {
                    s_lo = ~0ull;
                    s_hi = SX32(n);
                } else {
                    s_lo = SX32(n / d);
                    s_hi = SX32(n % d);
                }
                used += COST_DIV;
                break;
            }
            case 28: {
                __int128 r = (__int128)(s64)gpr[RS] * (s64)gpr[RT];
                s_lo = (u64)r;
                s_hi = (u64)(r >> 64);
                used += COST_DMULT;
                break;
            }
            case 29: {
                unsigned __int128 r = (unsigned __int128)gpr[RS] * gpr[RT];
                s_lo = (u64)r;
                s_hi = (u64)(r >> 64);
                used += COST_DMULT;
                break;
            }
            case 30: {
                s64 n = (s64)gpr[RS], d = (s64)gpr[RT];
                if (d == 0) {
                    s_lo = n < 0 ? 1 : ~0ull;
                    s_hi = (u64)n;
                } else if (d == -1) {
                    s_lo = 0 - (u64)n;
                    s_hi = 0;
                } else {
                    s_lo = (u64)(n / d);
                    s_hi = (u64)(n % d);
                }
                used += COST_DDIV;
                break;
            }
            case 31: {
                u64 n = gpr[RS], d = gpr[RT];
                if (d == 0) {
                    s_lo = ~0ull;
                    s_hi = n;
                } else {
                    s_lo = n / d;
                    s_hi = n % d;
                }
                used += COST_DDIV;
                break;
            }
            case 32: {
                s32 r;
                if (unlikely(__builtin_sadd_overflow((s32)gpr[RS], (s32)gpr[RT], &r))) goto ex_ov;
                gpr[RD] = SX32(r);
                break;
            }
            case 33: gpr[RD] = SX32((u32)gpr[RS] + (u32)gpr[RT]); break;
            case 34: {
                s32 r;
                if (unlikely(__builtin_ssub_overflow((s32)gpr[RS], (s32)gpr[RT], &r))) goto ex_ov;
                gpr[RD] = SX32(r);
                break;
            }
            case 35: gpr[RD] = SX32((u32)gpr[RS] - (u32)gpr[RT]); break;
            case 36: gpr[RD] = gpr[RS] & gpr[RT]; break;
            case 37: gpr[RD] = gpr[RS] | gpr[RT]; break;
            case 38: gpr[RD] = gpr[RS] ^ gpr[RT]; break;
            case 39: gpr[RD] = ~(gpr[RS] | gpr[RT]); break;
            case 42: gpr[RD] = (s64)gpr[RS] < (s64)gpr[RT]; break;
            case 43: gpr[RD] = gpr[RS] < gpr[RT]; break;
            case 44: {
                long long r;
                if (unlikely(__builtin_saddll_overflow((long long)gpr[RS], (long long)gpr[RT], &r))) goto ex_ov;
                gpr[RD] = (u64)r;
                break;
            }
            case 45: gpr[RD] = gpr[RS] + gpr[RT]; break;
            case 46: {
                long long r;
                if (unlikely(__builtin_ssubll_overflow((long long)gpr[RS], (long long)gpr[RT], &r))) goto ex_ov;
                gpr[RD] = (u64)r;
                break;
            }
            case 47: gpr[RD] = gpr[RS] - gpr[RT]; break;
            case 48: TRAP_IF((s64)gpr[RS] >= (s64)gpr[RT]); break;
            case 49: TRAP_IF(gpr[RS] >= gpr[RT]); break;
            case 50: TRAP_IF((s64)gpr[RS] < (s64)gpr[RT]); break;
            case 51: TRAP_IF(gpr[RS] < gpr[RT]); break;
            case 52: TRAP_IF(gpr[RS] == gpr[RT]); break;
            case 54: TRAP_IF(gpr[RS] != gpr[RT]); break;
            case 56: gpr[RD] = gpr[RT] << SA; break;
            case 58: gpr[RD] = gpr[RT] >> SA; break;
            case 59: gpr[RD] = (u64)((s64)gpr[RT] >> SA); break;
            case 60: gpr[RD] = gpr[RT] << (SA + 32); break;
            case 62: gpr[RD] = gpr[RT] >> (SA + 32); break;
            case 63: gpr[RD] = (u64)((s64)gpr[RT] >> (SA + 32)); break;
            default: goto ex_ri;
            }
            break;

        case 1: { /* REGIMM */
            s64 s = (s64)gpr[RS];
            switch (RT) {
            case 0:  BRANCH(s < 0); break;
            case 1:  BRANCH(s >= 0); break;
            case 2:  BRANCH_LIKELY(s < 0); break;
            case 3:  BRANCH_LIKELY(s >= 0); break;
            case 8:  TRAP_IF(s >= (s64)SIMM); break;
            case 9:  TRAP_IF((u64)s >= SIMM); break;
            case 10: TRAP_IF(s < (s64)SIMM); break;
            case 11: TRAP_IF((u64)s < SIMM); break;
            case 12: TRAP_IF((u64)s == SIMM); break;
            case 14: TRAP_IF((u64)s != SIMM); break;
            case 16: gpr[31] = SX32(npc); BRANCH(s < 0); break;
            case 17: gpr[31] = SX32(npc); BRANCH(s >= 0); break;
            case 18: gpr[31] = SX32(npc); BRANCH_LIKELY(s < 0); break;
            case 19: gpr[31] = SX32(npc); BRANCH_LIKELY(s >= 0); break;
            default: goto ex_ri;
            }
            break;
        }

        case 3: gpr[31] = SX32(npc); /* fallthrough */
        case 2:
            npc = (pc & 0xF0000000u) | ((inst & 0x03FFFFFFu) << 2);
            nds = 1;
            break;

        case 4:  BRANCH(gpr[RS] == gpr[RT]); break;
        case 5:  BRANCH(gpr[RS] != gpr[RT]); break;
        case 6:  BRANCH((s64)gpr[RS] <= 0); break;
        case 7:  BRANCH((s64)gpr[RS] > 0); break;
        case 8: {
            s32 r;
            if (unlikely(__builtin_sadd_overflow((s32)gpr[RS], (s32)(s16)inst, &r))) goto ex_ov;
            gpr[RT] = SX32(r);
            break;
        }
        case 9:  gpr[RT] = SX32((u32)gpr[RS] + (u32)SIMM); break;
        case 10: gpr[RT] = (s64)gpr[RS] < (s64)SIMM; break;
        case 11: gpr[RT] = gpr[RS] < SIMM; break;
        case 12: gpr[RT] = gpr[RS] & ZIMM; break;
        case 13: gpr[RT] = gpr[RS] | ZIMM; break;
        case 14: gpr[RT] = gpr[RS] ^ ZIMM; break;
        case 15: gpr[RT] = SX32(inst << 16); break;

        case 16: { /* COP0 */
            u32 st = cop0[C0_STATUS];
            if (unlikely(!(st & ST_CU0) && (st & 0x1Eu) == 0x10u)) {
                exc_ce = 0;
                goto ex_cpu;
            }
            if (RS & 0x10) {
                switch (inst & 63) {
                case 1: tlb_read(); break;
                case 2: tlb_write(cop0[C0_INDEX]); break;
                case 6:
                    count_sync(used);
                    tlb_write(random_value());
                    break;
                case 8: tlb_probe(); break;
                case 24:
                    if (st & ST_ERL) {
                        pc = cop0[C0_ERROREPC];
                        cop0[C0_STATUS] = st & ~ST_ERL;
                    } else {
                        pc = cop0[C0_EPC];
                        cop0[C0_STATUS] = st & ~ST_EXL;
                    }
                    npc = pc + 4;
                    s_llbit = 0;
                    tc_validate();
                    update_irq();
                    break;
                default: goto ex_ri;
                }
                ftag = FETCH_INVALID;
            } else {
                switch (RS) {
                case 0:
                case 1: gpr[RT] = SX32(cop0_read((int)RD, used)); break;
                case 4:
                case 5:
                    cop0_write((int)RD, (u32)gpr[RT], used);
                    ftag = FETCH_INVALID;
                    break;
                default: goto ex_ri;
                }
            }
            break;
        }

        case 17: /* COP1 */
            NEED_CU1;
            switch (RS) {
            case 0: gpr[RT] = SX32(fget_w(FS_)); break;
            case 1: gpr[RT] = fget_l(FS_); break;
            case 2: gpr[RT] = RD == 31 ? SX32(s_fcr31) : RD == 0 ? 0xA00u : 0; break;
            case 4: fset_w(FS_, (u32)gpr[RT]); break;
            case 5: fset_l(FS_, gpr[RT]); break;
            case 6:
                if (RD == 31) {
                    s_fcr31 = (u32)gpr[RT] & FCR31_MASK;
                    host_rm_apply((int)(s_fcr31 & 3));
                    if (s_fcr31 & (((s_fcr31 & 0xF80u) << 5) | FC_E)) {
                        exc_code = EXC_FPE;
                        goto ex_code;
                    }
                }
                break;
            case 8: {
                int c = !!(s_fcr31 & FCR_COND);
                int want = (inst >> 16) & 1;
                if (inst & (1u << 17)) BRANCH_LIKELY(c == want);
                else                   BRANCH(c == want);
                break;
            }
            case 16: case 17: case 20: case 21: {
                int extra;
                switch (RS) {
                case 16: extra = cop1_s(inst); break;
                case 17: extra = cop1_d(inst); break;
                case 20: extra = cop1_w(inst); break;
                default: extra = cop1_l(inst); break;
                }
                if (unlikely(extra < 0)) {
                    exc_code = EXC_FPE;
                    goto ex_code;
                }
                used += extra;
                break;
            }
            default: goto ex_ri;
            }
            break;

        case 18: NEED_CU2; break;

        case 20: BRANCH_LIKELY(gpr[RS] == gpr[RT]); break;
        case 21: BRANCH_LIKELY(gpr[RS] != gpr[RT]); break;
        case 22: BRANCH_LIKELY((s64)gpr[RS] <= 0); break;
        case 23: BRANCH_LIKELY((s64)gpr[RS] > 0); break;
        case 24: {
            long long r;
            if (unlikely(__builtin_saddll_overflow((long long)gpr[RS], (long long)(s16)inst, &r))) goto ex_ov;
            gpr[RT] = (u64)r;
            break;
        }
        case 25: gpr[RT] = gpr[RS] + SIMM; break;

        case 26: { /* LDL */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 0);
            used += COST_MEM;
            u32 sh = (va & 7) * 8;
            u64 m = mem_r64(pa & ~7u);
            gpr[RT] = (gpr[RT] & ((1ull << sh) - 1)) | (m << sh);
            break;
        }
        case 27: { /* LDR */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 0);
            used += COST_MEM;
            u32 sh = (7 - (va & 7)) * 8;
            u64 m = mem_r64(pa & ~7u);
            gpr[RT] = (gpr[RT] & ~(~0ull >> sh)) | (m >> sh);
            break;
        }

        case 32: LOAD(1, s8,  gpr[RT] = (u64)(s64)v_, ram[pa], n64_read8);
        case 33: LOAD(2, s16, gpr[RT] = (u64)(s64)v_, rd16(ram + pa), n64_read16);
        case 34: { /* LWL */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 0);
            used += COST_MEM;
            u32 sh = (va & 3) * 8;
            u32 m = mem_r32(pa & ~3u);
            gpr[RT] = SX32(((u32)gpr[RT] & ((1u << sh) - 1)) | (m << sh));
            break;
        }
        case 35: LOAD(4, s32, gpr[RT] = (u64)(s64)v_, rd32(ram + pa), n64_read32);
        case 36: LOAD(1, u8,  gpr[RT] = v_, ram[pa], n64_read8);
        case 37: LOAD(2, u16, gpr[RT] = v_, rd16(ram + pa), n64_read16);
        case 38: { /* LWR */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 0);
            used += COST_MEM;
            u32 sh = (3 - (va & 3)) * 8;
            u32 m = mem_r32(pa & ~3u);
            gpr[RT] = SX32(((u32)gpr[RT] & ~(0xFFFFFFFFu >> sh)) | (m >> sh));
            break;
        }
        case 39: LOAD(4, u32, gpr[RT] = v_, rd32(ram + pa), n64_read32);

        case 40: STORE(1, (u8)gpr[RT], ram[pa] = (u8)gpr[RT], n64_write8);
        case 41: STORE(2, (u16)gpr[RT], wr16(ram + pa, (u16)gpr[RT]), n64_write16);
        case 42: { /* SWL */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 1);
            used += COST_MEM;
            u32 sh = (va & 3) * 8;
            u32 mask = 0xFFFFFFFFu >> sh;
            u32 aligned = pa & ~3u;
            mem_w32(aligned, (mem_r32(aligned) & ~mask) | ((u32)gpr[RT] >> sh));
            break;
        }
        case 43: STORE(4, (u32)gpr[RT], wr32(ram + pa, (u32)gpr[RT]), n64_write32);
        case 44: { /* SDL */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 1);
            used += COST_MEM;
            u32 sh = (va & 7) * 8;
            u64 mask = ~0ull >> sh;
            u32 aligned = pa & ~7u;
            mem_w64(aligned, (mem_r64(aligned) & ~mask) | (gpr[RT] >> sh));
            break;
        }
        case 45: { /* SDR */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 1);
            used += COST_MEM;
            u32 sh = (7 - (va & 7)) * 8;
            u32 aligned = pa & ~7u;
            mem_w64(aligned, (mem_r64(aligned) & ((1ull << sh) - 1)) | (gpr[RT] << sh));
            break;
        }
        case 46: { /* SWR */
            u32 va = EA, pa;
            TRANSLATE(va, pa, 1);
            used += COST_MEM;
            u32 sh = (3 - (va & 3)) * 8;
            u32 aligned = pa & ~3u;
            mem_w32(aligned, (mem_r32(aligned) & ((1u << sh) - 1)) | ((u32)gpr[RT] << sh));
            break;
        }
        case 47: break; /* CACHE */

        case 48: { /* LL */
            ADDR(4, 0);
            gpr[RT] = SX32(mem_r32(pa));
            s_llbit = 1;
            cop0[C0_LLADDR] = pa >> 4;
            break;
        }
        case 49: /* LWC1 */
            NEED_CU1;
            {
                ADDR(4, 0);
                fset_w(FT_, mem_r32(pa));
            }
            break;
        case 50: case 54: case 58: case 62: NEED_CU2; break;
        case 52: { /* LLD */
            ADDR(8, 0);
            gpr[RT] = mem_r64(pa);
            s_llbit = 1;
            cop0[C0_LLADDR] = pa >> 4;
            break;
        }
        case 53: /* LDC1 */
            NEED_CU1;
            {
                ADDR(8, 0);
                fset_l(FT_, mem_r64(pa));
            }
            break;
        case 55: LOAD(8, u64, gpr[RT] = v_, rd64(ram + pa), n64_read64);
        case 56: { /* SC */
            ADDR(4, 1);
            if (s_llbit) mem_w32(pa, (u32)gpr[RT]);
            gpr[RT] = s_llbit;
            s_llbit = 0;
            break;
        }
        case 57: /* SWC1 */
            NEED_CU1;
            {
                ADDR(4, 1);
                mem_w32(pa, fget_w(FT_));
            }
            break;
        case 60: { /* SCD */
            ADDR(8, 1);
            if (s_llbit) mem_w64(pa, gpr[RT]);
            gpr[RT] = s_llbit;
            s_llbit = 0;
            break;
        }
        case 61: /* SDC1 */
            NEED_CU1;
            {
                ADDR(8, 1);
                mem_w64(pa, fget_l(FT_));
            }
            break;
        case 63: STORE(8, gpr[RT], wr64(ram + pa, gpr[RT]), n64_write64);

        default: goto ex_ri;
        }
        continue;

    ex_ri:
        exc_code = EXC_RI;
        goto ex_code;
    ex_ov:
        exc_code = EXC_OV;
        goto ex_code;
    ex_cpu:
        pc = exception_enter(EXC_CPU, exc_ce, cur, ids, 0x180);
        goto ex_done;
    ex_addr:
        cop0[C0_BADVADDR] = fault_va;
        /* fallthrough */
    ex_code:
        pc = exception_enter(exc_code, 0, cur, ids, 0x180);
        goto ex_done;
    ex_tlb:
        pc = tlb_fault(fault_kind, fault_wr, fault_va, cur, ids);
    ex_done:
        npc = pc + 4;
        nds = 0;
    }

    count_sync(used);
    cop0[C0_RANDOM] = random_value();
    gpr[0] = 0;
    s_pc = pc;
    s_npc = npc;
    s_delay = nds;
    host_rm_apply(0);
    return used;
}

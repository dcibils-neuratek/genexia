/*
 * ps1_mdec.c - the movie decoder: run-length codes into 8x8 blocks, the
 * game's own quantization and IDCT tables, then YUV to 15 or 24-bit pixels in
 * 16x16 macroblocks for the game to copy into VRAM.
 */
#include <stdlib.h>
#include "ps1.h"

static u8 qt_y[64], qt_c[64];
static s16 scale[64];
static u32 cmd_word, want, have;
static u16 *in;
static size_t in_cap;
static u8 *out;
static size_t out_len, out_at, out_cap;
static int enable_in, enable_out;
static u32 status_block;

static const u8 zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63 };
static u8 zagzig[64];

int mdec_has_output(void) { return out_at < out_len; }

static inline int s10(u16 v) { return (int)((s16)(v << 6) >> 6); }

static void idct(s16 *blk) {
    s32 tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int z = 0; z < 8; z++) sum += (s64)blk[y + z * 8] * scale[x + z * 8];
            tmp[x + y * 8] = (s32)((sum + 0x8000) >> 16);
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            s64 sum = 0;
            for (int z = 0; z < 8; z++) sum += (s64)tmp[y + z * 8] * scale[x + z * 8];
            s32 v = (s32)((sum + 0x8000) >> 16);
            blk[x + y * 8] = v < -128 ? -128 : v > 127 ? 127 : v;
        }
}

/* one block of run-length codes; false when the input has run out */
static int block(const u16 **p, const u16 *end, s16 *blk, const u8 *qt) {
    memset(blk, 0, 64 * sizeof *blk);
    u16 n;
    do { if (*p >= end) return 0; n = *(*p)++; } while (n == 0xfe00);
    const int q = (n >> 10) & 63;
    int k = 0;
    int val = s10(n) * qt[0];
    for (;;) {
        if (q == 0) val = s10(n) * 2;
        val = val < -0x400 ? -0x400 : val > 0x3ff ? 0x3ff : val;
        blk[q ? zagzig[k] : k] = val;
        if (*p >= end) break;
        n = *(*p)++;
        k += ((n >> 10) & 63) + 1;
        if (k > 63) break;
        val = (s10(n) * qt[k] * q + 4) / 8;
    }
    idct(blk);
    return 1;
}

static void emit(u8 b) {
    if (out_len == out_cap) { out_cap = out_cap ? out_cap * 2 : 1 << 16; out = realloc(out, out_cap); }
    out[out_len++] = b;
}

static void decode(void) {
    if (!zagzig[1]) for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = i;
    const int depth = (cmd_word >> 27) & 3, sign = (cmd_word >> 25) & 1, bit15 = (cmd_word >> 26) & 1;
    const u16 *p = in, *end = in + have * 2;
    out_len = out_at = 0;
    s16 cr[64], cb[64], y[4][64];
    while (p < end) {
        if (depth >= 2) {
            if (!block(&p, end, cr, qt_c) || !block(&p, end, cb, qt_c)) break;
            int ok = 1;
            for (int i = 0; i < 4 && ok; i++) ok = block(&p, end, y[i], qt_y);
            if (!ok) break;
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 16; px++) {
                    const int c = (px >> 1) + (py >> 1) * 8;
                    const int R0 = cr[c], B0 = cb[c];
                    const int G = (-22520 * B0 - 46812 * R0) >> 16, R = (91881 * R0) >> 16, B = (116130 * B0) >> 16;
                    const int Y = y[(py >> 3) * 2 + (px >> 3)][(px & 7) + (py & 7) * 8];
                    int r = Y + R, g = Y + G, b = Y + B;
                    r = r < -128 ? -128 : r > 127 ? 127 : r;
                    g = g < -128 ? -128 : g > 127 ? 127 : g;
                    b = b < -128 ? -128 : b > 127 ? 127 : b;
                    if (!sign) { r ^= 0x80; g ^= 0x80; b ^= 0x80; }
                    if (depth == 2) { emit(r); emit(g); emit(b); }
                    else {
                        const u16 v = ((r & 0xff) >> 3) | ((g & 0xff) >> 3) << 5 | ((b & 0xff) >> 3) << 10 | bit15 << 15;
                        emit(v); emit(v >> 8);
                    }
                }
        } else {        /* monochrome, one block at a time */
            if (!block(&p, end, y[0], qt_y)) break;
            for (int i = 0; i < 64; i++) {
                int v = y[0][i];
                if (!sign) v ^= 0x80;
                if (depth == 1) emit(v);
                else if (i & 1) { out[out_len - 1] |= (v & 0xf0); } else emit((v & 0xff) >> 4);
            }
        }
    }
}

static void param(u32 w) {
    const u32 op = cmd_word >> 29;
    if (op == 1) {
        if (have * 2 + 2 > in_cap) { in_cap = in_cap ? in_cap * 2 : 1 << 16; in = realloc(in, in_cap * sizeof *in); }
        in[have * 2] = w; in[have * 2 + 1] = w >> 16;
    } else if (op == 2) {
        u8 *t = have < 16 ? qt_y + have * 4 : qt_c + (have - 16) * 4;
        for (int i = 0; i < 4; i++) t[i] = w >> (8 * i);
    } else if (op == 3) {
        scale[have * 2] = (s16)w; scale[have * 2 + 1] = (s16)(w >> 16);
    }
    if (++have == want) {
        if (op == 1) { decode(); dma_mdec_ready(); }
        want = 0;
    }
}

void mdec_write(int reg, u32 v) {
    if (reg == 0) {
        if (want) { param(v); return; }
        cmd_word = v;
        have = 0;
        switch (v >> 29) {
        case 1: want = v & 0xffff; break;
        case 2: want = v & 1 ? 32 : 16; break;
        case 3: want = 32; break;
        default: want = 0;
        }
        return;
    }
    if (v & 0x80000000) { want = have = 0; out_len = out_at = 0; cmd_word = 0; }
    enable_in = (v >> 30) & 1;
    enable_out = (v >> 29) & 1;
}

u32 mdec_read(int reg) {
    if (reg == 0) return mdec_dma_out();
    u32 s = 0;
    if (out_at >= out_len) s |= 1u << 31;
    if (want) s |= 1u << 29;
    if (enable_in && want) s |= 1u << 28;
    if (enable_out && out_at < out_len) s |= 1u << 27;
    s |= ((cmd_word >> 25) & 15) << 23;
    s |= status_block << 16;
    s |= (want ? (want - have - 1) : 0xffff) & 0xffff;
    return s;
}

void mdec_dma_in(u32 w) { if (want) param(w); }

u32 mdec_dma_out(void) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) if (out_at < out_len) v |= (u32)out[out_at++] << (8 * i);
    return v;
}

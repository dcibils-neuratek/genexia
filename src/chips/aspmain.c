/*
 * aspmain.c - does the work of the audio microcode Mario 64 hands the RSP: walks
 * the command list and mixes in plain C. The real thing costs a G4 far too much
 * in emulated vector code. Same 4 KB of DMEM addresses as the microcode.
 */
#include <string.h>
#include "n64.h"

#define R32(p) ((u32)(p)[0] << 24 | (u32)(p)[1] << 16 | (u32)(p)[2] << 8 | (p)[3])
#define R16(p) ((u16)((p)[0] << 8 | (p)[1]))
#define W16(p, v) ((p)[0] = (u8)((v) >> 8), (p)[1] = (u8)(v))

static u8 mem[0x1000];
#define S(a) ((s16)R16(&mem[(a) & 0xFFE]))
#define SET(a, v) W16(&mem[(a) & 0xFFE], (u16)(v))

static u32 seg[16], loop_addr;
static int a_in, a_out, a_count, dry_right, wet_left, wet_right, a_dry, a_wet;
static s16 vol[2], target[2];
static s32 rate[2];
static s16 book[0x88];
static s16 lut[256];
static int lut_ok;

static inline u32 addr(u32 a) { return (seg[a >> 24 & 15] + (a & 0xFFFFFF)) & 0x7FFFFF; }
static inline s16 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
static inline s16 dr16(u32 pa) { return (s16)R16(&n64_rdram[pa & 0x7FFFFF]); }
static inline void dw16(u32 pa, s32 v) { W16(&n64_rdram[pa & 0x7FFFFF], (u16)v); }

/* The resampler's filter is data in the microcode; find it by its first entries. */
static void find_lut(void) {
    const u32 d = R32(&n64_sp_mem[0xFC0 + 0x18]) & 0x7FFFFF;
    for (u32 i = 0; i < 0x1000; i += 2)
        if (R32(&n64_rdram[d + i]) == 0x0C3966ADu && R32(&n64_rdram[d + i + 4]) == 0x0D46FFDFu) {
            for (int k = 0; k < 256; k++) lut[k] = dr16(d + i + k * 2);
            lut_ok = 1;
            return;
        }
}

static void a_adpcm(int flags, u32 pa) {
    s16 l1, l2;
    int in = a_in, out = a_out, n = (a_count + 31) & ~31;
    if (flags & 1) l1 = l2 = 0;
    else {
        const u32 from = flags & 2 ? loop_addr : pa;
        l1 = dr16(from + 28); l2 = dr16(from + 30);
    }
    /* the block starts with the 16 samples kept from last time */
    if (flags & 1) memset(&mem[out & 0xFFF], 0, 32);
    else memcpy(&mem[out & 0xFFF], &n64_rdram[(flags & 2 ? loop_addr : pa) & 0x7FFFFF], 32);
    out += 32;
    for (; n > 0; n -= 32) {
        const u8 hdr = mem[in++ & 0xFFF];
        const int scale = hdr >> 4, shift = scale < 12 ? 12 - scale : 0;
        const s16 *b1 = &book[(hdr & 15) * 16], *b2 = b1 + 8;
        s16 e[16];
        for (int i = 0; i < 8; i++) {
            const u8 c = mem[in++ & 0xFFF];
            e[i * 2] = (s16)((c & 0xF0) << 8) >> shift;
            e[i * 2 + 1] = (s16)((c & 0x0F) << 12) >> shift;
        }
        for (int part = 0; part < 2; part++) {
            const s16 *x = e + part * 8;
            s16 o[8];
            for (int i = 0; i < 8; i++) {
                s32 acc = (s32)x[i] << 11;
                acc += b1[i] * l1 + b2[i] * l2;
                for (int j = 0; j < i; j++) acc += b2[i - 1 - j] * x[j];
                o[i] = clamp16(acc >> 11);
            }
            for (int i = 0; i < 8; i++) { SET(out, o[i]); out += 2; }
            l1 = o[6]; l2 = o[7];
        }
    }
    memcpy(&n64_rdram[pa & 0x7FFFFF], &mem[(out - 32) & 0xFFF], 32);
}

static void a_resample(int flags, int pitch, u32 pa) {
    int ip = (a_in & 0xFFF) - 8, op = a_out & 0xFFF, n = ((a_count + 15) & ~15) >> 1;
    u32 acc = 0;
    if (flags & 1) { for (int i = 0; i < 4; i++) SET(ip + i * 2, 0); }
    else {
        for (int i = 0; i < 4; i++) SET(ip + i * 2, dr16(pa + i * 2));
        acc = (u16)dr16(pa + 8);
    }
    pitch <<= 1;
    for (int i = 0; i < n; i++, op += 2) {
        const s16 *c = &lut[((acc >> 10) & 0x3F) * 4];
        s32 a = 0;
        for (int k = 0; k < 4; k++) a += (S(ip + k * 2) * c[k] + 0x4000) >> 15;
        SET(op, clamp16(a));
        acc += (u32)pitch;
        ip += (int)(acc >> 16) * 2;
        acc &= 0xFFFF;
    }
    for (int i = 0; i < 4; i++) dw16(pa + i * 2, S(ip + i * 2));
    dw16(pa + 8, (s32)acc);
}

static inline s32 rd32s(u32 pa) { return (s32)R32(&n64_rdram[pa & 0x7FFFFF]); }
static inline void wr32s(u32 pa, s32 v) { dw16(pa, v >> 16); dw16(pa + 2, v); }

static void a_envmixer(int flags, u32 pa) {
    const int aux = flags & 8;
    s32 value[2], tgt[2], seq[2], r[2];
    int dry = a_dry, wet = a_wet;
    if (flags & 1) {
        for (int i = 0; i < 2; i++) {
            value[i] = vol[i] * 65536; tgt[i] = target[i] * 65536;
            r[i] = rate[i]; seq[i] = (s32)vol[i] * rate[i];
        }
    } else {
        /* the last eight gains of each ramp, high then low words; the newest counts */
        value[0] = (s32)((u32)(u16)dr16(pa + 0x0E) << 16 | (u16)dr16(pa + 0x1E));
        value[1] = (s32)((u32)(u16)dr16(pa + 0x2E) << 16 | (u16)dr16(pa + 0x3E));
        tgt[0] = dr16(pa + 0x40) * 65536; r[0] = rd32s(pa + 0x42);
        tgt[1] = dr16(pa + 0x46) * 65536; r[1] = rd32s(pa + 0x48);
        dry = dr16(pa + 0x4C); wet = dr16(pa + 0x4E);
        for (int i = 0; i < 2; i++) seq[i] = (s32)(((s64)value[i] * r[i]) >> 16);
    }
    const int n = a_count >> 1;
    int in = a_in, dl = a_out, dr = dry_right, wl = wet_left, wr = wet_right;
    for (int y = 0; y < n; y += 8) {
        s32 step[2];
        for (int i = 0; i < 2; i++) step[i] = (s32)((seq[i] - (s64)value[i]) >> 3);
        for (int x = 0; x < 8; x++) {
            s32 g[2];
            for (int i = 0; i < 2; i++) {
                value[i] += step[i];
                const int done = step[i] <= 0 ? value[i] <= tgt[i] : value[i] >= tgt[i];
                if (done) { value[i] = tgt[i]; step[i] = 0; }
                g[i] = (s16)(value[i] >> 16);
            }
            const s32 sm = S(in);
            const s32 gd[2] = { clamp16((g[0] * dry + 0x4000) >> 15), clamp16((g[1] * dry + 0x4000) >> 15) };
            SET(dl, clamp16(S(dl) + ((sm * gd[0] + 0x4000) >> 15)));
            SET(dr, clamp16(S(dr) + ((sm * gd[1] + 0x4000) >> 15)));
            if (aux) {
                const s32 gw[2] = { clamp16((g[0] * wet + 0x4000) >> 15), clamp16((g[1] * wet + 0x4000) >> 15) };
                SET(wl, clamp16(S(wl) + ((sm * gw[0] + 0x4000) >> 15)));
                SET(wr, clamp16(S(wr) + ((sm * gw[1] + 0x4000) >> 15)));
                wl += 2; wr += 2;
            }
            in += 2; dl += 2; dr += 2;
        }
        for (int i = 0; i < 2; i++) seq[i] = (s32)(((s64)seq[i] * r[i]) >> 16);
    }
    dw16(pa + 0x0E, value[0] >> 16); dw16(pa + 0x1E, value[0]);
    dw16(pa + 0x2E, value[1] >> 16); dw16(pa + 0x3E, value[1]);
    dw16(pa + 0x40, tgt[0] >> 16); wr32s(pa + 0x42, r[0]);
    dw16(pa + 0x46, tgt[1] >> 16); wr32s(pa + 0x48, r[1]);
    dw16(pa + 0x4C, dry); dw16(pa + 0x4E, wet);
}

void asp_run(void) {
    u32 a = R32(&n64_sp_mem[0xFC0 + 0x30]) & 0x7FFFFF;
    const u32 end = a + R32(&n64_sp_mem[0xFC0 + 0x34]);
    if (!lut_ok) find_lut();
    for (; a < end; a += 8) {
        const u32 w0 = R32(&n64_rdram[a]), w1 = R32(&n64_rdram[a + 4]);
        const int flags = w0 >> 16 & 0xFF;
        switch (w0 >> 24) {
        case 1: a_adpcm(flags, addr(w1)); break;
        case 2: { const int d = w0 & 0xFFF & ~3; int n = ((w1 & 0xFFFF) + 3) & ~3; if (d + n > 0x1000) n = 0x1000 - d; memset(&mem[d], 0, (size_t)n); break; }
        case 3: a_envmixer(flags, addr(w1)); break;
        case 4: memcpy(&mem[a_in & 0xFFC], &n64_rdram[addr(w1) & ~3u], (size_t)((a_count + 3) & ~3)); break;
        case 5: a_resample(flags, w0 & 0xFFFF, addr(w1)); break;
        case 6: memcpy(&n64_rdram[addr(w1) & ~3u], &mem[a_out & 0xFFC], (size_t)((a_count + 3) & ~3)); break;
        case 7: seg[w1 >> 24 & 15] = w1 & 0xFFFFFF; break;
        case 8:
            if (flags & 8) { dry_right = w0 & 0xFFFF; wet_left = w1 >> 16; wet_right = w1 & 0xFFFF; }
            else { a_in = w0 & 0xFFFF; a_out = w1 >> 16; a_count = w1 & 0xFFFF; }
            break;
        case 9:
            if (flags & 8) { a_dry = (s16)w0; a_wet = (s16)w1; }
            else {
                const int lr = flags & 2 ? 0 : 1;
                if (flags & 4) vol[lr] = (s16)w0;
                else { target[lr] = (s16)w0; rate[lr] = (s32)w1; }
            }
            break;
        case 10: memmove(&mem[w1 >> 16 & 0xFFF], &mem[w0 & 0xFFF], (size_t)((w1 + 15) & 0xFFF0)); break;
        case 11: {
            const u32 pa = addr(w1);
            for (u32 i = 0; i < (w0 & 0xFFFF) / 2 && i < 0x88; i++) book[i] = dr16(pa + i * 2);
            break;
        }
        case 12: {
            const s32 gain = (s16)w0;
            int in = w1 >> 16, out = w1 & 0xFFFF;
            for (int n = ((a_count + 31) & ~31) >> 1; n > 0; n--, in += 2, out += 2) SET(out, clamp16(S(out) + ((S(in) * gain) >> 15)));
            break;
        }
        case 13: {
            int l = w1 >> 16, r = w1 & 0xFFFF, o = a_out;
            s16 t[0x800];
            const int n = ((a_count + 15) & ~15) >> 1;
            for (int i = 0; i < n; i++) { t[i * 2] = S(l + i * 2); t[i * 2 + 1] = S(r + i * 2); }
            for (int i = 0; i < n * 2; i++) { SET(o, t[i]); o += 2; }
            break;
        }
        case 15: loop_addr = addr(w1); break;
        }
    }
}

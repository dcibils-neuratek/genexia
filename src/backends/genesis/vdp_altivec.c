/*
 * vdp_altivec.c - layer priority for the classic Mac, 16 pixels at a time.
 *
 * Built on its own for the G4, and called only after the machine says it has
 * AltiVec. The result is a colour number per pixel, or 0 for the backdrop:
 * a layer's colour 0 is transparent, so no layer gives that number. Constants
 * are made in registers, because lvx silently rounds a misaligned address and
 * nothing promises the alignment of loaded data here.
 */
#include "genesis.h"

typedef unsigned char v16 __attribute__((vector_size(16)));
typedef signed char s16v __attribute__((vector_size(16)));
/* opaque, so the compiler can't turn it back into a load */
#define SPLAT(n) ({ v16 v_; __asm__("vspltisb %0," #n : "=v"(v_)); v_; })
#define PICK(mask, yes, no) (((yes) & (mask)) | ((no) & ~(mask)))

/* all pointers on 16-byte boundaries; n rounds up to a multiple of 16 */
void vdp_compose_altivec(u8 *out, const u8 *A, const u8 *B, const u8 *S, int n) {
    const v16 ones = SPLAT(-1), zero = ones ^ ones;
    const v16 low = SPLAT(15);
    v16 colour;                       /* 0x3F: the compiler would make this a loaded constant */
    __asm__("vsrb %0,%1,%2" : "=v"(colour) : "v"(ones), "v"(SPLAT(2)));
    for (int x = 0; x < n; x += 16) {
        const v16 a = *(const v16 *)(A + x), b = *(const v16 *)(B + x), s = *(const v16 *)(S + x);
        const v16 aon = (v16)((a & low) != zero), bon = (v16)((b & low) != zero), son = (v16)((s & low) != zero);
        const v16 ahi = aon & (v16)((s16v)a < (s16v)zero), bhi = bon & (v16)((s16v)b < (s16v)zero), shi = son & (v16)((s16v)s < (s16v)zero);
        v16 c = b & bon;
        c = PICK(aon, a, c);
        c = PICK(son, s, c);
        c = PICK(bhi, b, c);
        c = PICK(ahi, a, c);
        c = PICK(shi, s, c);
        *(v16 *)(out + x) = c & colour;
    }
}

/* The same with shadow and highlight on. bank says which palette the colour
 * comes from: 0 normal, 1 shadow, 2 highlight. Colours 0x3E and 0x3F of a
 * winning sprite are operators on the plane below it, not colours. */
void vdp_compose_shadow_altivec(u8 *out, u8 *bank, const u8 *A, const u8 *B, const u8 *S, int n) {
    const v16 ones = SPLAT(-1), zero = ones ^ ones;
    const v16 low = SPLAT(15), one = SPLAT(1);
    const v16 two = SPLAT(2), fourteen = SPLAT(14);
    v16 colour, op_light;              /* 0x3F, 0x3E: see above about constants */
    __asm__("vsrb %0,%1,%2" : "=v"(colour) : "v"(ones), "v"(two));
    __asm__("vxor %0,%1,%2" : "=v"(op_light) : "v"(colour), "v"(one));
    for (int x = 0; x < n; x += 16) {
        const v16 a = *(const v16 *)(A + x), b = *(const v16 *)(B + x), s = *(const v16 *)(S + x);
        const v16 aon = (v16)((a & low) != zero), bon = (v16)((b & low) != zero), son = (v16)((s & low) != zero);
        const v16 ahi = aon & (v16)((s16v)a < (s16v)zero), bhi = bon & (v16)((s16v)b < (s16v)zero);
        const v16 spri = (v16)((s16v)s < (s16v)zero);
        const v16 shadow = ~(v16)((s16v)(a | b) < (s16v)zero);
        v16 pc = b & bon;
        pc = PICK(aon, a, pc);
        pc = PICK(bhi, b, pc);
        pc = PICK(ahi, a, pc);
        pc &= colour;
        const v16 win = son & (spri | ~(ahi | bhi)), sc = s & colour;
        const v16 light = win & (v16)(sc == op_light), dark = win & (v16)(sc == colour);
        const v16 sprite = win & ~(light | dark);
        const v16 bright = spri | (v16)((s & low) == fourteen);
        v16 k = shadow & one;
        k = PICK(sprite & bright, zero, k);
        k = PICK(light, ~shadow & two, k);
        k = PICK(dark, one, k);
        *(v16 *)(out + x) = PICK(sprite, sc, pc);
        *(v16 *)(bank + x) = k;
    }
}

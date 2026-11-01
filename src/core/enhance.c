/*
 * enhance.c - picture enhancements applied after the console has drawn.
 *
 * Scale2x (Andrea Mazzoleni's EPX refinement) doubles pixel art by rounding
 * its diagonals without inventing colours; run twice it gives 4x.
 */
#include "enhance.h"

void enhance_scale2x(const u32 *src, int stride, int w, int h, u32 *dst) {
    for (int y = 0; y < h; y++) {
        const u32 *r = src + (size_t)y * stride;
        const u32 *up = y ? r - stride : r, *dn = y + 1 < h ? r + stride : r;
        u32 *o0 = dst + (size_t)y * 2 * (w * 2), *o1 = o0 + w * 2;
        for (int x = 0; x < w; x++) {
            u32 e = r[x], b = up[x], h_ = dn[x], d = r[x ? x - 1 : 0], f = r[x + 1 < w ? x + 1 : x];
            int edge = b != h_ && d != f;
            o0[x * 2]     = edge && d == b ? d : e;
            o0[x * 2 + 1] = edge && b == f ? f : e;
            o1[x * 2]     = edge && d == h_ ? d : e;
            o1[x * 2 + 1] = edge && h_ == f ? f : e;
        }
    }
}

/* more saturation, about 30%, around each pixel's own brightness */
void enhance_vivid(u32 *px, int n) {
    for (int i = 0; i < n; i++) {
        int r = (px[i] >> 16) & 255, g = (px[i] >> 8) & 255, b = px[i] & 255;
        int l = (r * 77 + g * 150 + b * 29) >> 8;
        r += (r - l) * 77 >> 8; g += (g - l) * 77 >> 8; b += (b - l) * 77 >> 8;
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        px[i] = (u32)r << 16 | (u32)g << 8 | (u32)b;
    }
}

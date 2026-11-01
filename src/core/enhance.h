/* enhance.h - picture enhancements applied after the console has drawn. */
#ifndef ENHANCE_H
#define ENHANCE_H
#include "genesis.h"
void enhance_scale2x(const u32 *src, int stride, int w, int h, u32 *dst);
void enhance_vivid(u32 *px, int n);
#endif

/* What psp_ge.c hands the GPU: transformed vertices and the PSP's draw state. */
#ifndef PSP_METAL_H
#define PSP_METAL_H
#include <stdint.h>

typedef struct { float pos[4], uv[2], pad[2], col[4]; } MtlVert;
typedef struct {
    void *tex;                       /* from mtl_texture, or NULL */
    uint32_t tex_target;             /* a render target's address when it is the texture */
    float uv_scale[2];
    int clamp_u, clamp_v, linear;
    int tfunc, use_alpha, doubled;
    float env[3];
    int alpha_test, alpha_func, alpha_ref, alpha_mask;
    int blend, src, dst, eq;
    float fix_a[3], fix_b[3];
    int depth_test, depth_func, depth_write;
    int write_mask;                  /* bit 0 red, 1 green, 2 blue, 3 alpha */
    int sx1, sy1, sx2, sy2;
} MtlState;

void mtl_open(void *sdl_window, int scale, int msaa);
void mtl_draw(uint32_t target, const MtlState *s, const MtlVert *v, int count);
void mtl_texture(void **slot, int w, int h, int pitch, const uint32_t *rgba);
int mtl_is_target(uint32_t addr);
int mtl_drawn(uint32_t addr);
void mtl_present(uint32_t addr, const uint32_t *cpu_rgba);
void mtl_shot(uint32_t addr, const char *path);
#endif
